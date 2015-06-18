/* Creates a compressed image, given a file as an argument.
 * (c)1999 Paul `Rusty' Russell.  GPL.
 *
 * CHANGELOG:
 *
 * * Sun, 22 Dec 2013 15:10:49 +0100 Daniel Plasa <dplasa@gmail.com>
 * - Add support for v3 cloop files adding lz4, lzo and xz compression support
 * - Removed dependency to advancecomp project since the only part used from
 *   that is 7zip compression - which was removed as xz compression performs
 *   better in every respect.
 * - Limited v2 backward compatibility to only operate on real files
 *   (remove separate header production / temp file (re-)usage and other mess)
 *   since that is no longer needed with v3 cloop format and straightens the
 *   code further up.
 * - Allow source file to be any size (why restrict the user here?)
 * - Changed network data handling with respect to cloop v3 files. Also
 *   the way remote nodes are specified has changed. A node can now be a host
 *   or even a network. The client will then discover remote nodes, using UDP
 *   broadcast to all given node addresses.
 *   A server will reply via UDP to such discovery requests. The client will
 *   start threads for every received discovery reply.
 * - Complete overhaul, rewrote many parts including thread handling,
 *   adding more STL and using come C++11 features
 * - Renamed to cloop_util.cc as the original name was misleading. It NEVER
 *   did advfs or create_compressed_fs, it just compressed one given file into
 *   another (cloop) file. Of course if file happened to be a block device or
 *   an image of a file system - well - you could say so, but for the sake of
 *   clarity I don't.
 * - Merged in the old command line tools cloop_suspend and
 *   extract_compressed_fs since the latter was only aware of v2 cloop files
 *
 * * Wed, 02 Aug 2006 02:01:42 +0200 Eduard Bloch <blade@debian.org>
 * - cleanup, fixed memory leak in doLocalCompression with best compression
 * - simplified the control flow a lot, it was overdesigned. Kept one ring
 *   buffer for data passing and aside buffers for temp. data storage.
 *
 * * Mon, 29 Aug 2005 15:20:10 CEST 2005 Eduard Bloch <blade@debian.org>
 * - a complete overhaul, rewrote most parts using STL and C++ features,
 *   portable data types etc.pp.
 * - using multiple threads, input/output buffers, various strategies for
 *   temporary data storage, options parsing with getopt, and: network enabled
 *   server/client modes, distributing the compression jobs to many nodes
 *
 * * Mon Feb 28 20:45:14 CET 2005 Sebastian Schmidt <yath@yath.eu.org>
 * - Added -t command line option to use a temporary file for the compressed
 *   blocks instead of saving it in cb_list.
 * - Added information message before the write of the compressed image.
 * * Sat Deb 14 22:49:20 CEST 2004 Christian Leber
 * - Changed to work with the advancecomp packages, so that it's
 *   better algorithms may be used
 * * Sun Okt 26 01:05:29 CEST 2003 Klaus Knopper
 * - Changed format of index pointers to network byte order
 * * Sat Sep 29 2001 Klaus Knopper <knopper@knopper.net>
 * - changed compression to Z_BEST_COMPRESSION,
 * * Sat Jun 17 2000 Klaus Knopper <knopper@knopper.net>
 * - Support for reading file from stdin,
 * - Changed Preamble.
 * * Sat Jul 28 2001 Klaus Knopper <knopper@knopper.net>
 * - cleanup and gcc 2.96 / glibc checking
 */

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <endian.h>
#include <fcntl.h>
#include <sys/select.h>

// Cloop (V2 and) V3 definition header - also used by the
// kernel module cloop.ko
#include "cloop.h"

// all compression algorithms as defined in cloop.h with names
// and some helper classes around
#include "compression_helpers.h"

// pthread wrapper class
#include "Pthread.h"

// client server stuff
#include "clientserver.h"

// network stuff
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#if defined(__linux__)
#include <sys/socketvar.h>
#endif

// for server
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

#include <iostream>
#include <iomanip>
#include <cstdio>
#include <string>
#include <list>
#include <map>
#include <vector>
#include <limits>

#include "debug.h"
using namespace std;

// CLOOP v3 Format is defined in cloop.h

#ifdef __CYGWIN__
typedef uint64_t loff_t;
#endif

#if defined(linux) || defined(__linux__)
#include <asm/byteorder.h>
#define htonll(x) __cpu_to_be64(x)

#else // not linux
#ifndef be64toh
#if BYTE_ORDER == LITTLE_ENDIAN
static __inline __uint64_t __bswap64(__uint64_t _x)
{
	return ((_x >> 56) | ((_x >> 40) & 0xff00) | ((_x >> 24) & 0xff0000) |
			((_x >> 8) & 0xff000000) | ((_x << 8) & ((__uint64_t)0xff << 32)) |
			((_x << 24) & ((__uint64_t)0xff << 40)) |
			((_x << 40) & ((__uint64_t)0xff << 48)) | ((_x << 56)));
}
#define htonll(x)      __bswap64(x)
#else // BIG ENDIAN
#define htonll(x) x
#endif
#endif /* !be64toh */

#endif // linux

#ifndef MAX
	#define MAX(a,b) ((b)>(a)) ? (b) : (a)
#endif

#ifndef MSG_WAITALL
#define MSG_WAITALL 0
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// some globals, easier option passing
unsigned short defPort = 3103;

// compression strategy, default: trade size for speed
compressionStrategy_t strategy = cs_SMART;

// desired compression Level
int desiredLevel = 0;

// number of compression threads to be created
uint32_t workThreads = 0;

// number of blocks to be read in advance in memory
uint32_t poolSize = 0;

bool beQuiet(false);
int beVerbose(0);
bool compressorsSpecified(false), strategySpecified(false);
bool blockSizeSpecified(false), remoteSpecified(false);

// mode this program is running with
enum workMode_t
{
	mode_UNSPECIFIED = 0,
	mode_COMPRESS = (1<<0),
	mode_SERVER = (1<<1),
	mode_SERVERDETACH = (1<<2),
	//
	mode_SUSPEND = (1<<3),
	mode_UNCOMPRESS = (1<<4)
};
uint32_t workMode = mode_UNSPECIFIED;

// produce what cloop file version
uint32_t fileVersion = 0;

// info about all (un)compressed blocks
#define defaultBlockSize "8K"
size_t blockSize = 0;       // uncompressed size
uint32_t numBlocks = 0; 	// how many uncompressed blocks
uint64_t originalSize = 0;  // original size of file
vector<size_t> lengths;     // length of all compressed blocks
vector<uint32_t> flags;     // compressed block flags

// struct to hold uncompressed blocks read by the fetcher thread
// each block is blockSize bytes
map<size_t, void*> uncompressedBlocks;

// struct to hold compressed data to be written by the output thread
// block size is given in vector<size_t> lengths
map<size_t, void*> compressedBlocks;

// list of remote nodes to connect to
list<node_address> remoteNodes;

// threads to do mt-compression
class cThread: public Pthread
{
public:
	enum threadState
	{
		t_IDLE,       // initial state
		t_SLEEPWAIT,  // sleep waiting for data to process
		t_GET,        // getting buffer with uncompressed data
		t_SEND,		  // send data to remote node
		t_REMOTEWAIT, // wait for the remote note to finish
		t_REMOTEFAIL, // something happened on the way to remote node
		t_PROCESS,    // doing local compression
		t_STORE,	  // do store here
		t_INVALID	  // must never get here!
	};
	cThread(const char* name) : Pthread(name), state(t_IDLE){}
	virtual ~cThread() {}

	virtual threadState idle()
	{
		return t_INVALID;
	}
	virtual threadState sleepWait()
	{
		return t_INVALID;
	}
	virtual threadState get()
	{
		return t_INVALID;
	}
	virtual threadState send()
	{
		return t_INVALID;
	}
	virtual threadState remoteWait()
	{
		return t_INVALID;
	}
	virtual threadState remoteFail()
	{
		return t_INVALID;
	}
	virtual threadState process()
	{
		return t_INVALID;
	}
	virtual threadState store()
	{
		return t_INVALID;
	}

	virtual void loop()
	{
		switch (state)
		{
		case t_IDLE:
			state = idle();
			break;

		case t_SLEEPWAIT:
			state = sleepWait();
			break;

		case t_GET:
			state = get();
			break;

		case t_SEND:
			state = send();
			break;

		case t_REMOTEWAIT:
			state = remoteWait();
			break;

		case t_REMOTEFAIL:
			state = remoteFail();
			break;

		case t_PROCESS:
			state = process();
			break;

		case t_STORE:
			state = store();
			break;

		case t_INVALID:
			// FIXME for valgrind
			state = t_IDLE;
			break;

		default:
			die(name() << "[" << id() << "]::loop() state machine ran into invalid state " << state, EINVAL);
			break;
		}
	}

	inline threadState getState() const { return state; }
protected:
	threadState state;

	// pair that holds current uncompressed block of data
	pair<size_t, void*> inBuf;

	static Pconditional input;			// all workers wait for input
	static Pconditional inputFetch; 	// input fetcher thread waits for this
	static Pconditional output;  		// output writer thread waits for this
};
Pconditional cThread::input;
Pconditional cThread::inputFetch;
Pconditional cThread::output;

// a local compression thread
class CompressThread: public cThread
{
protected:
	// results is a list of compressed blocks that were rendered by all selected
	// compressors
	// it will be sorted by the desired strategy target
	std::list<compressionResult_t> results;

public:
	CompressThread(const char* name = "worker") : cThread(name)
	{
	}
	virtual ~CompressThread() {}

	virtual threadState idle()
	{
		return t_SLEEPWAIT;
	}

	virtual threadState sleepWait()
	{
		// the local compression thread waits for a piece of data to be read by the fetcher thread
		// then processes the data
		for (;;)
		{
			lock();
			size_t i = uncompressedBlocks.size();
			unlock();
			if (i > 0) break;
			sleepUntil(input);
		}
		tDEBUG(3, "::sleepWait() success, found data to process");
		return t_GET;
	}

	virtual threadState get()
	{
		// always just take the first buffer with uncompressed data
		// since the map of uncompressed buffers is sorted this will also
		// be the block with the smallest block number
		// that seems sensible as the output writer thread needs to write the
		// blocks ordered
		lock();
		if (uncompressedBlocks.size())
		{
			inBuf = *uncompressedBlocks.begin();
			uncompressedBlocks.erase(uncompressedBlocks.begin());
			if (uncompressedBlocks.size() < poolSize) wake(inputFetch);
			unlock();
			return t_SEND;
		}
		else
		{
			unlock();
			return t_SLEEPWAIT;
		}
	}

	virtual threadState send()
	{
		// local mode, skip send and process data locally
		return t_PROCESS;
	}

	virtual threadState process()
	{
		// inBuf holds a pair of <block number, void* [uncompressed data]>
		tDEBUG(3, "::process() doing local compression, Block: " << inBuf.first << ", compressors " << allCompressors.list_compressors());
		compress(allCompressors, strategy, blockSize);
		return t_STORE;
	}

	virtual threadState store()
	{
		lock();

		// check what we have and store
		if (inBuf.first > flags.size())
		{
			// we need to enlarge the flags, lengths
			flags.resize(inBuf.first+1);
			lengths.resize(inBuf.first+1);
		}

		if (cs_NONE == strategy || 0 == results.size())
		{
			// store inBuf only
			flags[inBuf.first] = 0;
			lengths[inBuf.first] = blockSize;
			if (cs_NONE != strategy)
				tDEBUG(3, "storing incompressible block " << inBuf.first << " uncompressed");
		}
		else
		{
			// store the compressed result from the used compressor

			// the best compression result is the first list entry
			compressionResult_t &best = results.front();

			// we free the inBuf memory that was holding the uncompressed data
			// CAUTION: check if inBuf.second == results.front().buf
			// since NetClientCompressThread uses only inBuf.second for all
			// purposes
			if (best.buf != inBuf.second) free(inBuf.second);

			// copy flags and length information
			flags[inBuf.first] = best.cid;  // save used compressor id to flags
			lengths[inBuf.first] = best.compressedSize; // save compressed size
			inBuf.second = (void*)best.move_buffer(); 	// move buffer from compressor out
			allCompressors.incUsecounter(best);			// update statistics
			tDEBUG(3, "::store() moving block " << inBuf.first << " (" << allCompressors.name(best.cid) << ", size " << best.compressedSize << ") to writer queue");
		}

		// put compressed block into compressed_blocks map
		compressedBlocks.insert(inBuf);

		unlock();

		// signal output thread there is data to be written
		tDEBUG(3, "::store() wake(output)");
		wake(output);

		// and we are idle again, ready for next data block
		return t_IDLE;
	}

	void compress(compressorMap_t &Compressors, compressionStrategy_t strategy, size_t blockSize)
	{
		// clean out previous compression results
		results.clear();

		if (cs_NONE != strategy)
		{
			// compress with all given algorithms, time decompression if needed
			bool time_uncompress = (cs_BESTTIME == strategy || cs_SMART == strategy);
			Compressors.compressAll(results, inBuf.second, blockSize, time_uncompress);

			// sort by chosen strategy
			if (cs_BESTTIME == strategy)
				results.sort(compressionResult_t::compare_time);
			else if (cs_BESTSIZE == strategy)
				results.sort(compressionResult_t::compare_size);
			else if (cs_SMART == strategy)
				results.sort(compressionResult_t::compare_smart);
			else
				die("Unknown strategy " << strategy << " - this is a program BUG!", EINVAL);
		}
	}
};


// common base for the network threads
// server or client
class Net
{
public:
	Net(int _fd=0) : sockfd(_fd) {}
	virtual ~Net()
	{
		close();
	}

	virtual void close()
	{
		if (sockfd)
		{
			::shutdown(sockfd, SHUT_RDWR);
			::close(sockfd);
			sockfd=0;
		}
	}

	size_t write(const void* src, size_t srclen)
	{
		size_t sent = 0;
		for (; sent < srclen; )
		{
			size_t tmp = ::send(sockfd, ((const unsigned char*)src)+sent, srclen-sent, MSG_NOSIGNAL|MSG_WAITALL);
			if ((size_t)-1 == tmp || 0 == tmp)
			{
				DEBUG(3, "Net::write() send failed: " << strerror(errno));
				return sent;
			}
			sent += tmp;
		}
		DEBUG(3, "Net::write(" << srclen << ") = " << sent << " done!");
		return srclen;
	}

	size_t read(void * dest, size_t destlen)
	{
		for (size_t bytes_read = 0; bytes_read < destlen; )
		{
			size_t tmp = ::recv(sockfd, ((unsigned char*)dest) + bytes_read, destlen - bytes_read, MSG_NOSIGNAL|MSG_WAITALL);
			if (tmp == 0)
				// somebody closed the connection
				return bytes_read;
			if ((size_t)-1 == tmp)
			{
				// error while reading
				DEBUG(3, "Net::read() recv failed: " << strerror(errno));
				return bytes_read;
			}
			DEBUG(3, "Net::read() " << tmp << " bytes");
			bytes_read += tmp;
		}
		return destlen;
	}

protected:
	int sockfd;
};

// common base for the network compression threads
// server or client
class NetCompressThread : public CompressThread, public Net
{
public:
	NetCompressThread(const char* name) : CompressThread(name) {}

protected:
	NetRequestV3 request;
	NetReplyV3 reply;
};

// a compression thread that sends its data as client to a remote node
// constructor receives a file descriptor of a socket to a v3 server
class NetClientCompressThread: public NetCompressThread
{
public:
	NetClientCompressThread(int _fd) : NetCompressThread("client")
	{
		sockfd = _fd;
	}

	void setSocket(int _fd)
	{
		// if this thread is going to be revived from the outside - we use
		// this function. Will only be called if the thread does not run so
		// the state machine loop() is not being run.
		sockfd = _fd;
		state = t_IDLE;
	}

	virtual threadState idle()
	{
		// check if something went wrong the last time and if the socket was
		// closed in consequence - in that case we terminate the thread
		if (sockfd <= 0)
		{
			cancel();
			// return does not matter - this thread is going to die
			// and the state machine will no longer be run
			return t_IDLE;
		}
		return t_SLEEPWAIT;
	}

	virtual threadState send()
	{
		request.dataSize = htonl(blockSize);
		request.strategy = htonl(strategy);
		request.compressorMask = htonl(allCompressors.availableMask());
		request.level = htonl(desiredLevel);
		// FIXME request.workMode
		if (sizeof (request) != write(&request, sizeof(request)))
		{
			tDEBUG(3, "::send()ing request failed, revert to local compression");
			return t_REMOTEFAIL;
		}
		// send out the buffer of data
		tDEBUG(3, "::send()ing Block " << inBuf.first << ", " << blockSize<< " bytes");
		if (blockSize != write(inBuf.second, blockSize))
			return t_REMOTEFAIL;

		return t_REMOTEWAIT;
	}

	virtual threadState remoteWait()
	{
		// all sent, wait for remote node to answer!
		tDEBUG(3, "::remoteWait()ing for an answer...");
		if (sizeof(reply) != read(&reply, sizeof(reply)))
		{
			tDEBUG(4, "::remoteWait() no reply, fail/close");
			return t_REMOTEFAIL;
		}
		// translate back to host order
		reply.dataSize = ntohl(reply.dataSize);
		reply.protocolVersion = ntohl(reply.protocolVersion);
		reply.compressor = ntohl(reply.compressor);
		reply.result = ntohl(reply.result);

		// if the remote is not talking the right protocol with us... skip!
		if (reply.protocolVersion < PROTOCOL_VERSION_MIN  ||  reply.protocolVersion > PROTOCOL_VERSION_MAX)
		{
			tDEBUG(3, "::remoteWait() answer, but protocol version " << reply.protocolVersion << " is unknown, bailing out.");
			return t_REMOTEFAIL;
		}
		// check result to see wether further data can be expected
		if (C_INCOMPRESSIBLE == reply.result || 0 == (size_t)reply.dataSize)
		{
			tDEBUG(3, "::remoteWait() block " << inBuf.first << " is incompressible - store it");
			results.clear();
			return t_STORE;
		}
		else if (C_OK != reply.result)
		{
			// we try local again
			tDEBUG(3, "::remoteWait() block " << inBuf.first << " failed remote compression, result " << reply.result);
			return t_REMOTEFAIL;
		}

		// read compressed block from remote node
		if (reply.dataSize != read(inBuf.second, reply.dataSize))
			return t_REMOTEFAIL;

		// build a 'result' list
		results.clear(); results.emplace_front();
		results.front().compressedSize = reply.dataSize;
		results.front().res = (compressorResult_t)reply.result;
		results.front().buf = inBuf.second;
		results.front().cid = reply.compressor;
		return t_STORE;
	}

	virtual threadState remoteFail()
	{
		// something went wrong on the way to the server or timed out or whatever
		// we compress locally and discard this connection
		tDEBUG(3, "::remoteFail() RemoteCompression(Block: " << inBuf.first << ") failed, doing it local now and end this connection");

		// close() will close the socket and set the sockfd to zero - this will end this thread
		// next time it is in idle()
		close();

		// return t_PROCESS so that the next state will do local compression
		return t_PROCESS;
		// after this is done and the state becomes t_IDLE again, the thread will terminate
	}
};

// a compression thread that receives data over a socket from a client
// it compresses the data and sends it back to the client, then
// waits for more data from the client
class NetServerCompressThread: public NetCompressThread
{
private:
	compressorMap_t myCompressors;
public:
	NetServerCompressThread() : NetCompressThread("server"), myCompressors(allCompressors) {}

	void setSocket(int _fd)
	{
		sockfd = _fd;
		// ugly but needed
		state = t_SLEEPWAIT;
		// else the NetListenTCPThread might already have
		// run and will select this very same NetServerCompressThread again
		wakeAll(input);
	}

	virtual threadState idle()
	{
		// wait for someone to put us a valid file descriptor
		while (sockfd <= 0)
		{
			tDEBUG(3, "::idle() no sockfd sleeping!");
			wake(inputFetch);
			sleepUntil(input);
		}
		tDEBUG(3, "::idle() woke up, sockfs = " << sockfd);
		return t_SLEEPWAIT;
	}

	virtual threadState sleepWait()
	{
		//  wait on <sockfd> for a piece of data to arrive
		// read 8 bytes, that could be the request of a v2 client
		int32_t v2meta[2];
		if (sizeof(v2meta) != read(&v2meta, sizeof(v2meta)))
		{
			tDEBUG(3, "::sleepWait() no more data, closing");
			return t_REMOTEFAIL;
		}

		// check whether this is a v2 request or a v3+ request
		// a v3 client will always send CLOOP3_NET_SIGNATURE at the beginning
		// v2meta[1] would contain the v3 protocol version
		if (CLOOP3_NET_SIGNATURE == ntohl(v2meta[0]))
		{
			request.signature = CLOOP3_NET_SIGNATURE;
			if (PROTOCOL_VERSION_MIN < ntohl(v2meta[1])  || ntohl(v2meta[1]) > PROTOCOL_VERSION_MAX)
			{
				tDEBUG(3, "::sleepWait() requested protocol is " << request.protocolVersion << " - no idea howto, closing");
				return t_REMOTEFAIL;
			}

			request.protocolVersion = v2meta[1];
			// now read the rest of the request
			if (sizeof(request)-8 != read(&request.workMode, sizeof (request)-8))
			{
				tDEBUG(3, "::sleepWait() no valid v3 request, closing");
				return t_REMOTEFAIL;
			}
			// convert to host order
			request.ntoh();
		}
		else
		{
			// could be v2 request, check for sanity
			v2meta[0] = ntohl(v2meta[0]);	// v2 block_size
			v2meta[1] = ntohl(v2meta[1]);	// v2 method
			if (0 < v2meta[0] && v2meta[0] < 1024*1024 && -2 <= v2meta[1] && v2meta[1] <= 9)
			{
				// save v2 flag
				request.protocolVersion = 2;

				// evaluate the desired method, that is the zlib compression level (0..9)
				// or -1 to indicate 7z use or -2 that requests the best size which is
				// quite silly as zlib-9 or 7z would almost everytime be the result
				// since we dropped 7z we treat <0 as 9 and >=0 as its value
				request.strategy = cs_BESTSIZE;
				request.compressorMask = CLOOP3_COMPRESSOR_ZLIB;
				request.dataSize = v2meta[0];
				request.level = (v2meta[1] < 0 ? 9 : v2meta[1]);
			}
			else
			{
				tDEBUG(3, "::sleepWait() no valid v2 or v3 request, closing");
				return t_REMOTEFAIL;
			}
		}
		// FIXME request.workMode
		myCompressors.setAvailByMask(request.compressorMask);
		myCompressors.setLevel(request.level);
		tDEBUG(3, "::sleepWait() v" << request.protocolVersion << " request received, size " << request.dataSize << " bytes, strategy " << request.strategy << ", compressors '" << myCompressors.list_compressors() << "'");
		// now read the data to be compressed
		return t_GET;
	}

	virtual threadState get()
	{
		// change memory buffer size since each request might have a different blockSize
		if (inBuf.second)
			inBuf.second = realloc(inBuf.second, request.dataSize);
		else
			inBuf.second = malloc(request.dataSize);
		// read block from remote node
		if (request.dataSize  != read(inBuf.second, request.dataSize))
		{
			return t_REMOTEFAIL;
		}
		return t_PROCESS;
	}

	virtual threadState process()
	{
		// inBuf holds a pair of <???, void* uncompressed data>
		tDEBUG(3, "::process() doing local compression of remote Block with " << request.dataSize  << "bytes");
		compress(myCompressors, (compressionStrategy_t)request.strategy, request.dataSize);
		if (results.size())
		{
			tDEBUG(3, "::process() compressed with " << myCompressors.name(results.front().cid) << " size " << results.front().compressedSize);
		}
		return t_STORE;
	}

	virtual threadState store()
	{
		// build answer
		if (2 == request.protocolVersion)
		{
			if (0 == results.size())
			{
				tDEBUG(0, "::store INTERNAL error - this is a BUG!");
				return t_REMOTEFAIL;
			}
			else if (C_OK != results.front().res)
			{
				// incompressible or whatever - we cannot answer to a v2 client
				tDEBUG(3, "::store() compression failed - v2 bailing out");
				return t_REMOTEFAIL;
			}
			// v2, blocksize := compressed_size, method := request.level
			int32_t v2meta[2];
			v2meta[0] = htonl(results.front().compressedSize);
			v2meta[1] = htonl(request.level);
			if (sizeof(v2meta) != write(&v2meta, sizeof(v2meta)))
			{
				tDEBUG(3, "::store() sending v2meta failed");
				return t_REMOTEFAIL;
			}
		}
		else if (PROTOCOL_VERSION_CURRENT == request.protocolVersion)
		{
			if (0 == results.size())
			{
				// no compression done, just echo the uncompressed block
				reply.dataSize = htonl(request.dataSize);
				reply.result = htonl(C_OK);
				reply.compressor = htonl(0);
			}
			else
			{
				reply.dataSize = htonl(results.front().compressedSize);
				reply.result = htonl(results.front().res);
				reply.compressor = htonl(results.front().cid);
			}
			reply.protocolVersion = htonl(PROTOCOL_VERSION_CURRENT);

			tDEBUG(3, "::store() sending v3 reply header (" << sizeof reply << " bytes)");
			if (sizeof(reply) != write(&reply, sizeof(reply)))
				return t_REMOTEFAIL;
			if (results.size() && C_OK != results.front().res)
				// compression failed - no need to send data back
				return t_REMOTEFAIL;
		}
		// send out the buffer of data
		tDEBUG(3, "::store() sending Block reply " << ntohl(reply.dataSize) << " bytes");
		if (ntohl(reply.dataSize) != write(results.size() ? results.front().buf : inBuf.second, ntohl(reply.dataSize)))
			return t_REMOTEFAIL;

		// free compressed buffers allocated in results list
		results.clear();

		// wait for next block
		return t_SLEEPWAIT;
	}

	virtual threadState remoteFail()
	{
		// something went wrong on the way to the client or timed out or whatever
		// we just close this connection
		close();
		// we must free the inBuf memory allocated at beginning
		free(inBuf.second); inBuf.second = NULL;
		// if other memory was allocated during compression
		// by clearing results that will be free()d as well
		results.clear();
		return t_IDLE;
	}
};

// a thread that opens an UDP or TCP socket on <port> and
// listens for incoming connections
class NetListenThread: public cThread
{
public:
	NetListenThread(int port, int socktype, const char* _name) : cThread(_name)
	{
		// open an udp or tcp socket and bind to it
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = socktype;
		hints.ai_flags = AI_PASSIVE; // use my IP

		int rv=0;
		char pport[8]; snprintf((char*)&pport, sizeof(pport), "%d", port);
		if ((rv = getaddrinfo(NULL, pport, &hints, &servinfo)) != 0)
		{
			die("getaddrinfo: " << gai_strerror(rv), EIO);
		}

		// loop through all the results and bind to the first we can
		for(p = servinfo; p != NULL; p = p->ai_next)
		{
			if ((sockfd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
			{
				tDEBUG(3, "socket (typ " << p->ai_socktype << ") failed");
				continue;
			}
			tDEBUG(3, "socket " <<  sockfd << " for ai_family " << p->ai_family << "created.");

			int yes = 1;
			if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
			{
				die("setsockopt failed", errno);
			}

			if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1)
			{
				::close(sockfd);
				tDEBUG(3, "bind failed");
				continue;
			}
			break;
		}

		if (NULL == p)
		{
			die("failed to bind to any socket", EIO);
		}

		freeaddrinfo(servinfo); // all done with this structure
	}
protected:
	void *get_in_addr(struct sockaddr *sa)
	{
		// get sockaddr, IPv4 or IPv6:
	    if (sa->sa_family == AF_INET)
	        return &(((struct sockaddr_in*)sa)->sin_addr);
	    return &(((struct sockaddr_in6*)sa)->sin6_addr);
	}
	unsigned short get_in_port(struct sockaddr *sa)
	{
		// get port number, IPv4 or IPv6:
	    if (sa->sa_family == AF_INET)
	        return ((struct sockaddr_in*)sa)->sin_port;
	    return ((struct sockaddr_in6*)sa)->sin6_port;
	}
	int sockfd;  	// listen socket
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr; // connector's address information
	char their_name[INET6_ADDRSTRLEN];
};

// Listens on UDP for clients searching for compression nodes:
//
// A client would send an UDP packet containing a client2server_v3_discovery struct,
// with protocol_version set to the needed minimum protocol version
// The server answers with an UDP packet containing the same struct,
// with protocol_version set to the supported maximum protocol version
class NetListenUDPThread: public NetListenThread
{
protected:
	client2server_v3_discovery buf;
public:
	NetListenUDPThread(const char* name) : NetListenThread(defPort, SOCK_DGRAM, name)
	{
		// all done here
	}

	virtual threadState idle()
	{
		// direct to get()
		return t_GET;
	}

	virtual threadState get()
	{
		// just recv and answer
		socklen_t addr_len = sizeof their_addr;
		if (sizeof(buf) != ::recvfrom(sockfd, &buf, sizeof(buf), MSG_WAITALL|MSG_NOSIGNAL,
				(struct sockaddr *)&their_addr, &addr_len))
		{
			tDEBUG(3, "::get() failed:" << strerror(errno));
		}
		else
		{
			inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), their_name, sizeof their_name);
			unsigned short their_port = get_in_port((struct sockaddr *)&their_addr);

		    // check magic and min_protocol_version match
			buf.protocolVersion = ntohl(buf.protocolVersion);
		    if ((ntohl(buf.signature) == CLOOP3_NET_SIGNATURE) && (PROTOCOL_VERSION_MIN <= buf.protocolVersion) && (buf.protocolVersion <= PROTOCOL_VERSION_MAX))
		    {
		    	tDEBUG(3, "::get() discovery packet from " << their_name << ":" << their_port);
		    	return t_SEND;
		    }
		    else
				tDEBUG(3, "::get() dropping UDP packet from " << their_name << ":" << their_port );
		}
		return t_GET;
	}

	virtual threadState send()
	{
		// answer back
		buf.protocolVersion = htonl(PROTOCOL_VERSION_MAX);
    	// indicate number of concurrent connections to the client
    	buf.suggestedConnections = ntohl(workThreads);
    	// set destination port do defPort
    	if (AF_INET == their_addr.ss_family)
    		((sockaddr_in&)their_addr).sin_port = htons(defPort);
    	else
    		((sockaddr_in6&)their_addr).sin6_port = htons(defPort);

    	if (sizeof(buf) != ::sendto(sockfd, &buf, sizeof(buf), MSG_WAITALL|MSG_NOSIGNAL,
			(const struct sockaddr *)&their_addr, sizeof(their_addr)))
		{
			tDEBUG(3, "::send() reply failed: " << strerror(errno));
		}
		else
		{
			tDEBUG(3, "::send() discovery reply: ok");
		}
		return t_GET;
	}
};

// Listens on TCP for clients connecting for sending uncompressed blocks
//
// A client would be using a client2server_v3_request structure as block header.
// The server compresses the block of data following that header and send the results
// with a server2client_v3_reply header back.
class NetListenTCPThread: public NetListenThread
{
private:
	NetServerCompressThread * freeWorker;
	vector<NetServerCompressThread * > allWorker;
public:
	NetListenTCPThread(const char* name) : NetListenThread(defPort, SOCK_STREAM, name), freeWorker(NULL)
	{
		// use number of compression threads + 3 as backlog
		if (listen(sockfd, workThreads+3) == -1)
		{
			die("failed to listen", EIO);
		}

		// start needed compression threads
		for (int i=-workThreads; i; ++i)
			allWorker.push_back(new NetServerCompressThread);
	}

	virtual threadState idle()
	{
		// nothing, just go to sleepWait()
		return t_SLEEPWAIT;
	}

	virtual threadState sleepWait()
	{
		freeWorker = NULL;
		for (auto &i: allWorker)
		{
			tDEBUG(3, "NetServerCompressThread " << i->name() << i->id() << " status = " << i->getState());
			if (NetServerCompressThread::t_IDLE != i->getState())
				continue;

			freeWorker = i;
			return t_GET;
		}

		tDEBUG(3, "::sleepWait() no compression thread is idle, sleeping...");
		// FIXME
		usleep(1000000);
		//waitUntil(input_fetcher);
		return t_SLEEPWAIT;
	}

	virtual threadState get()
	{
		tDEBUG(3, "::get() Compression Thread " << freeWorker->name() <<"[" << freeWorker->id() << "] is idle, waiting for a new connections...");
		socklen_t sin_size = sizeof (their_addr);
		int new_fd = ::accept(sockfd, (struct sockaddr *) &their_addr, &sin_size);
		if (new_fd == -1)
		{
			tDEBUG(3, "::loop() accept failed");
			return t_SLEEPWAIT;
		}

		inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *) &their_addr), their_name, sizeof their_name);
		unsigned short their_port = get_in_port((struct sockaddr *) &their_addr);
		info("connection from " << their_name << ":" << their_port);
		freeWorker->setSocket(new_fd);

		return t_SLEEPWAIT;
	}
};

// Sends out discovery packets to remote nodes that can be used to compress data blocks
//
// The client broadcasts client2server_v3_request structures every 10 seconds
// FIXME / TODO: Discovery will be done using ipv4 udp broadcasts, ignoring all
// ipv6 networks that might be provided.
// For future releases this should be changed to also work with ipv6 multicast
class NetDiscoverUDPThread: public Net, public Pthread
{
	bool show_info;
public:
	NetDiscoverUDPThread() : Pthread("discover")
	{
		show_info = false;
		for (auto &n: remoteNodes)
		{
			if (n.af == AF_INET) continue;
			if (n.prefixLen != 128 )
			{
				warn("ipv6 support is limited, skipping all ipv6 nodes that needed detection by multicast");
				break;
			}
		}
		if ((sockfd = socket(PF_INET6, SOCK_DGRAM, 0)) == -1)
		{
			die("Cannot create UDP socket", errno);
		}
		int yes = 1;
		if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &yes,sizeof yes) == -1)
		{
			die("Cannot set broadcast", errno);
		}
	}

	virtual void loop()
	{
		// loop over all remoteNodes and send discovery request
		for (auto &n: remoteNodes)
		{
			// skip multicast ipv6
			if (AF_INET6 == n.af  && 128 != n.prefixLen) continue;

			// else (ipv4, ipv4 broadcast, ipv6)

			struct sockaddr_in6 remote6;
			struct sockaddr_in remote4;

			char oaddr[INET6_ADDRSTRLEN], baddr[INET6_ADDRSTRLEN];

			remote6.sin6_family = AF_INET6;     	// host byte order
			remote6.sin6_port = htons(defPort); 	// short, network byte order
			remote6.sin6_addr = n.address.ip6;
			remote4.sin_family = AF_INET;
			remote4.sin_port = htons(defPort);
			remote4.sin_addr = n.address.ip4;
			memset(remote4.sin_zero, 0, sizeof remote4.sin_zero);
			if (AF_INET == n.af && 32 != n.prefixLen)
			{
				uint64_t bcast = 1;
				bcast <<= (32-n.prefixLen);
				--bcast;
				//DEBUG(3, ":::: ip: " << hex << ntohl(*(uint32_t*)&(n.address.ip4)) << " bcast: " << bcast);
				bcast |= ntohl(*(uint32_t*)&(n.address.ip4));
				bcast = htonl(bcast);
				remote4.sin_addr.s_addr = bcast;
				// char buf[100];
				// inet_ntop(AF_INET, &remote4.sin_addr, buf, sizeof(buf));
				//DEBUG(3, ":::: result: " << hex << ntohl(bcast) << dec << "= Buf: " << buf);
			}
			inet_ntop(n.af, &n.address, oaddr, sizeof(oaddr));
			inet_ntop(n.af, (n.af == AF_INET ? (void*)&remote4.sin_addr : (void*)&remote6.sin6_addr), baddr, sizeof(baddr));
			if (!show_info)
			{
				info("Scanning " << baddr << " for remote nodes " << oaddr << "/" << n.prefixLen);
			}
			else
				tDEBUG(3, "::loop() sending discovery message for " << oaddr << "/" << n.prefixLen << " to " << baddr);

			// send out the discovery requests
			client2server_v3_discovery discovery;
			discovery.suggestedConnections = htonl(-1);

			if (AF_INET == n.af)
				::sendto(sockfd, &discovery, sizeof(discovery), MSG_WAITALL|MSG_NOSIGNAL, (struct sockaddr *)&remote4, sizeof remote4);
			else
				::sendto(sockfd, &discovery, sizeof(discovery), MSG_WAITALL|MSG_NOSIGNAL, (struct sockaddr *)&remote6, sizeof remote6);
		}
		show_info = true;

		// sleep 10s
		// TODO make this configurable via options
		usleep(10000000);
	}
};

// Receives discovery replies from remote nodes and starts NetClientCompressThreads
// for each discovered node
class NetDiscoverReplyThread : public NetListenUDPThread
{
private:
	map<std::string, NetClientCompressThread*> allNetworkers;
public:
	NetDiscoverReplyThread() : NetListenUDPThread("detect")
	{
		// all done here
	}

	virtual threadState send()
	{
		// first check if the discovery reply is not our own broadcast
		// we skip it and just get another one
		if ((uint32_t)-1 == ntohl(buf.suggestedConnections))
			return t_GET;

		// in buf we have a reply from a remote node
		inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), their_name, sizeof their_name);
		tDEBUG(3, "::send() create " << ntohl(buf.suggestedConnections) << " connections to " << their_name);

		for (uint32_t t = 0; t < ntohl(buf.suggestedConnections); ++t)
		{
			// see, if we have already a NetClientCompressThread for that server
			char s0[INET6_ADDRSTRLEN+10]; snprintf(s0, sizeof(s0), "%s+%u", their_name, t);
			map<std::string, NetClientCompressThread*>::iterator i = allNetworkers.find(s0);
			NetClientCompressThread* ncth = NULL;
			if (i != allNetworkers.end())
			{
				// check if that is still running
				if (0 == (i->second->getThreadState() & pts_STOP))
				{
					// this remote nod is still working - skipping
					continue;
				}
				// this remote_node died - try to re-start it
				ncth = i->second;
			}

			// we should start a NetClientCompressThread or re-use an existing one
			// either way, we need a new socket to connect to the remote node
			int new_sock = ::socket(their_addr.ss_family, SOCK_STREAM, 0);
			if (-1 == new_sock)
			{
				tDEBUG(3, "::send() got discovery reply but cannot create socket to connect");
				return t_GET;
			}

			if (AF_INET == their_addr.ss_family )
			{
				struct sockaddr_in &in4 = *(struct sockaddr_in*) &their_addr;
				in4.sin_port = htons(defPort);
			}
			else
			{
				struct sockaddr_in6 &in6 = *(struct sockaddr_in6*) &their_addr;
				in6.sin6_port = htons(defPort);
			}

			if (::connect(new_sock, (const sockaddr*)&their_addr, sizeof their_addr) < 0)
			{
				tDEBUG(3, "::send() got discovery reply but cannot connect to " << their_name << ": " << strerror(errno));
				::close(new_sock);
				return t_GET;
			}
			if (ncth == NULL)
			{
				// all is fine! Fire up a new thread
				ncth = new NetClientCompressThread(new_sock);
				std::pair<std::string, NetClientCompressThread*> p(s0, ncth);
				allNetworkers.insert(p);
			}
			else
			{
				ncth->setSocket(new_sock);
				ncth->start();
			}
		}

		// wait for more replies
		return t_GET;
	}
};

// This thread loads uncompressed data from the input file
// into memory and wakes up sleeping compression threads that waitUntil(input)
class LocalFileInputThread: public cThread
{
public:
	LocalFileInputThread(int _fd) :
			cThread("InputT"), fd(_fd)
	{
		// start with block number 0, so -1 as it is ++inBuf.first later...
		inBuf.first = -1;
		inBuf.second = NULL;
		// original_filesize will be overwritten to the real number of bytes read in
		originalSize = 0;
	}

	virtual ~LocalFileInputThread()
	{
		if (fd)
		{
			close(fd);
			fd = 0;
		}
		inBuf.first = -1;
		inBuf.second = NULL;
	}

	virtual void loop()
	{
		// read from the file <fd>

		for (;;)
		{
			lock();
			size_t i = uncompressedBlocks.size();
			unlock();
			if (i < poolSize) break;
			tDEBUG(3, "waiting for free pool...");
			sleepUntil(inputFetch);
		}
		// no we are allowed to read
		if (inBuf.second == NULL)
		{
			inBuf.second = malloc(blockSize);
			++inBuf.first;
		}
		unsigned char *ptr = (unsigned char*) inBuf.second;
		size_t rest = blockSize;
		while (rest > 0)
		{
			ssize_t r = ::read(fd, ptr, rest);
			if (r < 0)
				die("Input stream error.", EIO);
			ptr += r;
			rest -= r;
			originalSize += r;
			if (r == 0)
				break;
		}
		if (rest < blockSize)
			tDEBUG(3, "::loop() block " << inBuf.first << " with " << blockSize - rest << " bytes read");
		if (rest == blockSize)
		{
			// no input left - we quit the input thread
			tDEBUG(3, "::loop() no more input data, exiting");
			// adjust (if needed) or set at all the number of uncompressed input blocks
			numBlocks = inBuf.first;
			cancel();
			return;
		}
		if (rest > 0)
		{
			// padding with zeroes and making sure that the endmark will be set in the next block
			memset(ptr, 0, rest);
		}
		lock();
		uncompressedBlocks.insert(inBuf); // put read block in uncompressed map
		inBuf.second = NULL;               // force new buffer for next block
		unlock();
		tDEBUG(3, "::loop() wake() a compression thread");
		wake(input);                       // signal sleeping compression threads
	}
private:
	int fd;
};

// This thread writes compressed blocks to the outfile
// shows some statictis while doing it
class LocalFileOutputThread: public cThread
{
public:
	FILE * f;
	uint64_t total_compressed;
	uint32_t curr_block;
	double starttime, lastinfo;
	uint32_t next_block_info;

	LocalFileOutputThread(FILE* _outfh) :
		cThread("output"), f(_outfh), total_compressed(0), curr_block(0)
	{
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
		starttime = ts.tv_sec;
		starttime *=1.0e9;
		starttime += ts.tv_nsec;
		starttime *= 1.0e-9;
		lastinfo = starttime;
		next_block_info = numBlocks / 10;
	}
	virtual ~LocalFileOutputThread()
	{
		// do not fclose() the output file - it is needed later
		// we later need to write the index table and flags!
		if (f) fflush(f);
		f = NULL;
	}

	virtual void loop()
	{
		// write to the output file <f>
		for (;;)
		{
			lock();
			bool isoutput = (compressedBlocks.size() && compressedBlocks.begin()->first == curr_block);
			unlock();
			if (isoutput) break;
			tDEBUG(3, "::loop() waiting for compressed block " << curr_block);
			sleepUntil(output);
		}
		tDEBUG(3, "::loop() writing compressed block " << curr_block << " size " << lengths[curr_block]);
		lock();
		const void * buf = compressedBlocks.begin()->second;
		compressedBlocks.erase(compressedBlocks.begin());
		unlock();
		// write the buffer and free the memory afterwards
		size_t len = lengths[curr_block];
		if (fwrite(buf, 1, len, f) != len)
			die("Writting output", EIO);
		free((void*) buf);

		total_compressed += lengths[curr_block];

		// print some statistics
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
		double delta = ts.tv_sec;
		delta *= 1.0e9;
		delta += ts.tv_nsec;
		delta *= 1.0e-9;

		// if we were told to be verbose, every 10% of blocks completed or at the end
		// or if more than 10sec have elapsed
		if(!beQuiet && beVerbose && (delta>=lastinfo+10.0 || curr_block>= next_block_info || (curr_block+1)>=numBlocks))
		{
			next_block_info = curr_block + numBlocks/10;
			double num_processed = curr_block+1;
			double num_total = numBlocks;
			double fblock = blockSize;
			double todo = (num_total-num_processed) / num_processed;
			double ratio = 100.0F*double(total_compressed) / (num_processed*fblock);
			double speed = num_processed*fblock/(delta-starttime) / 1024.0;
			char sizec = 'K';
			if (speed > 1024.0) { speed /= 1024.0; sizec = 'M';}
			if (speed > 1024.0) { speed /= 1024.0; sizec = 'G';}

			if (ratio >99.94F)
				fprintf(stderr, "Blk# %5d, [avg. ratio 100.0%%],", curr_block);
			else
				fprintf(stderr, "Blk# %5d, [avg. ratio %5.02f%%],", curr_block, ratio);
			fprintf(stderr, " avg.speed: %5.01f %cb/s, ETA: %.1fs\n",
					speed, sizec, todo * (delta-starttime));
			lastinfo = delta;
		}

		if (++curr_block >= numBlocks)
		{
			// no more data to be written
			cancel();
		}
	}
};

uint32_t getWorkMode(const char *progname)
{
	const char* sep = strrchr(progname, '/');
	if (NULL == sep) sep = progname;
	else ++sep;
	if (0 == strcmp("cloop_suspend", sep) || 0 == strcmp("suspend", sep))
		return mode_SUSPEND;
	else if (0 == strcmp("cloop_create", sep) || 0 == strcmp("create", sep))
		return mode_COMPRESS;
	else if (0 == strcmp("cloop_uncompress", sep) || 0 == strcmp("uncompress", sep))
		return mode_UNCOMPRESS;
	return mode_UNSPECIFIED;
}

int usage(char *progname)
{
	#define COMMON_OPTIONS "h?"
	#define SUSPEND_OPTIONS COMMON_OPTIONS
	#define COMPRESS_OPTIONS COMMON_OPTIONS "23B:ldp:R:s:qvt:j:C:L:Z:"
	#define UNCOMPRESS_OPTIONS COMMON_OPTIONS "ldp:R:qvt:j:"

	cout << "Usage: " << progname << " ";

	if (mode_UNSPECIFIED == workMode)
	{
		cout << "command [options]" << endl;
		cout << "Command: create | suspend | uncompress." << endl;
		cout << "         create     - creates a cloop image from a file" << endl;
		cout << "         suspend    - suspends a mounted cloop image" << endl;
		cout << "         uncompress - uncompresses a cloop image" << endl;
		cout << "For more information call " << progname << " command -h" << endl;
		return 1;
	}

	uint32_t tmp_mode = getWorkMode(progname);
	if (mode_UNSPECIFIED == tmp_mode)
	{
		if (mode_SUSPEND == workMode) cout << "suspend ";
		else if (mode_COMPRESS == workMode) cout << "create ";
		else if (mode_UNCOMPRESS == workMode) cout << "uncompress ";
	}

	if (mode_SUSPEND == workMode)
	{
		cout << "DEVICE" << endl;
		cout << "       suspends and unlocks device until a new file is loaded via losetup" << endl;
		return 1;
	}
	else
	{
		cout << "[options] INFILE OUTFILE" << endl;
		cout << "Options:" << endl;
	}

	if (mode_COMPRESS ==  workMode)
	{
		cout << " -2    Create a cloop v2.0 compressed image" << endl;
		cout << " -3    Create a cloop v3 compressed image (default)" << endl;
		cout << " -B N  Set the block size to N (default: " << defaultBlockSize << ")" << endl;
		cout << " -l    Listening mode (as remote node)" << endl;
		cout << " -d    If run in listening mode: do not detach, run in foreground" << endl;
		cout << " -p M  Set listening port number to M (default " << defPort << ")" << endl;
		cout << " -s Q  Expect data with size Q from the input" << endl;
	}
	cout << " -R H  Use remote nodes H to compress/uncompress" << endl;
	cout << "       H can be a as hostname or ip adress or even a network" << endl <<
			"       given in IP/CIRD prefix." << endl;
	cout << " -q    Don't print periodic status messages to the console" << endl;
	cout << " -v    Verbose mode, print extra statistics" << endl;
	cout << "Performance tuning options:" << endl;
	cout << " -t T  Total number T of compressing threads (default: cpu cores+2)" << endl;
	cout << " -j U  Job pool size U (default: thread count+3)" << endl;

	if (mode_COMPRESS == workMode)
	{
		cout << " -C A  Select one or more compression algorithms from:" << endl
			 <<	"       none, any, zlib, lz4, lzo, xz" << endl;
		cout << " -L V  Specify desired compression level V (0..9) for some algorithms" << endl;
		cout << " -Z S  Compression strategy Z: none, size, time or smart:" << endl;
		cout << "       none: store only; size: try specified algorithms, keep smallest;"<< endl;
		cout << "       time: use fastest uncompressing algorithm (usually lz4 or lzo1x);" << endl;
		cout << "       smart: trade some decompression speed for better compression size" << endl;
		cout << endl;
		cout << "To use standard input/output '-' can be used as INFILE/OUTFILE. Passing \n"
				"the INPUT data size with -s may help.\n"
				"The size numbers can be declared with a suffix which acts as multiplier\n"
				"(K: KiB, k: KB, i: iso9660 block (2KiB); M: MiB; m: MB; G: GiB; g: GB)." << endl;
	}
	return (1);
}

uint64_t getsize(const char *text)
{
	if (!text)
		return 0;

	int map[] =
	{ 'k', 1000, 'K', 1024, 'm', 1000000, 'M', 1048576, 'g', 1000000000, 'G',
			1073741824, 'i', 2048, 0 };
	const char *mod = text + strlen(text) - 1;
	uint64_t fac = 0;
	if (*mod > '9')
	{
		for (int i = 0; map[i]; i += 2)
			if (map[i] == *mod)
				fac = map[i + 1];
		if (!fac)
			die("Unknown factor " << mod << " or bad size " << text, EINVAL);
	}
	else
		fac = 1;

	return fac * atoll(text);
}

inline bool is_pos_number(const char *text)
{
	for (; *text; )
		if (!isdigit(*text++))
			return false;
	return true;
}

void check_warn_processors()
{
// check number of workThreads
#ifdef _SC_NPROCESSORS_ONLN
if (workThreads > (uint32_t)sysconf(_SC_NPROCESSORS_ONLN)+2)
	warn(sysconf(_SC_NPROCESSORS_ONLN) << " processor core(s) detected, " << workThreads << " compression threads might actually slow overall time.");
else
	info( sysconf(_SC_NPROCESSORS_ONLN) << " processor core(s) detected, using " << workThreads << " compression threads.");
#endif
}

bool check_node(const char* src, list<node_address> &node_list)
{
	// check first if src is in address/prefixlen notation
	const char* prefix = strchr(src, '/');
	char tmpBuf[prefix ? prefix - src +1 : 1];
	const char* addr = src;
	if (prefix)
	{
		addr = (char*)&tmpBuf;
		strncpy((char*)addr, src, prefix -src);
		(char&)addr[prefix -src] = 0;
		++prefix;
	}

	// prefix is a number?
	if (prefix && !is_pos_number(prefix))
		return false;

	// check address itself
	struct addrinfo* result;
	struct addrinfo hints; memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_family = AF_UNSPEC;
	int err = getaddrinfo(addr, NULL, &hints, &result);
	if (err)
		return false;
//	DEBUG(3, "host |" << addr << "| translates to: ");
	bool found = false;
	for (struct addrinfo *p=result; p!=NULL; p=p->ai_next)
	{
		if (!(p->ai_family == AF_INET || p->ai_family == AF_INET6))
			continue;

		found = true;
		node_list.emplace_back();
		node_address& n = node_list.back();
		n.af = p->ai_family;
		if (p->ai_family == AF_INET)
		{
			struct sockaddr_in *ipv = (struct sockaddr_in *)p->ai_addr;
			n.address.ip4 = ipv->sin_addr;
			n.prefixLen = prefix ? atoi(prefix) : 32;
		}
		else // INET6
		{
			struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
			n.address.ip6 = ipv6->sin6_addr;
			n.prefixLen = prefix ? atoi(prefix) : 128;
		}
//		char ipstr[128];
//		inet_ntop(n.af, &n.address, ipstr, sizeof ipstr);
//		DEBUG(3, ipstr);
	}
	freeaddrinfo (result);
	return found;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SERVER operation mode
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int start_server()
{
	info("Starting server on port " << defPort << ((workMode & mode_SERVERDETACH) ? " in detached workMode" : ""));
	if ( ((workMode & mode_SERVERDETACH) && !fork()) || (workMode & mode_SERVER) )
	{
		// this is either the main or a fork()ed child process

		// The NetListenTCPThread will create a SOCK_STREAM socket()
		// bind() to it and start accept()ing connections
		// It will also create some NetServerCompressThread's
		// that will work on the accepted connections
		NetListenTCPThread tcplistener("tcplistener");

		// The NetListenUDPThread will create a SOCK_DGRAM socket()
		// and respond to discovery messages
		NetListenUDPThread udplistener("udplistener");

		// we wait for the listener to die - ideally this should never happen
		tcplistener.join();
	}
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SUSPEND operation mode
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int do_suspend(const char* devname)
{
	int fd = ::open(devname, O_RDONLY);

	if (fd < 0)
		die("Cannot open " << devname << ": " << strerror(errno), errno);

	if (::ioctl(fd, CLOOP_SUSPEND) < 0)
		die("Cannot suspend " << devname  << ": " << strerror(errno), errno);

	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Main
// Sorts out the mode of operation and starts accordingly
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv)
{
	// first determine how we are supposed / called to run
	workMode = getWorkMode(argv[0]);
	if (1 == argc) return (usage(argv[0]));  // no argument is always too short

	// if workMode is mode_UNSPECIFIC then check, if mode is first argument
	if (mode_UNSPECIFIED == workMode)
	{
		workMode = getWorkMode(argv[1]);
		if (mode_UNSPECIFIED == workMode || 2 == argc) // now: one argument is too short!
			return (usage(argv[0]));

		// shift args 1 left
		for (int i=2; i<argc; ++i)
		{
			argv[i-1] = argv[i];
		}
		--argc;
	}
	//
	// now we have a valid work mode and a proper argument list...
	//

#ifdef _SC_NPROCESSORS_ONLN
	workThreads = sysconf(_SC_NPROCESSORS_ONLN) + 2;
#else
	workThreads = 1;
#endif

	// parse in command line
	const char* validOptions  = (mode_UNCOMPRESS == workMode ? (const char*) UNCOMPRESS_OPTIONS :
			(mode_COMPRESS == workMode ? (const char*) COMPRESS_OPTIONS :
			(mode_SUSPEND == workMode ? (const char*) SUSPEND_OPTIONS : (const char*) COMMON_OPTIONS))
			);
	while (1)
	{
		int c = getopt(argc, argv, validOptions);
		if (c == -1)
			break;

		switch (c)
		{
		case 'U':
			// uncompress mode?
			if (mode_UNCOMPRESS == getWorkMode(argv[0]))
			{
				// started as cloop_uncompress and called with -U: messed up command line!
				exit(usage(argv[0]));
			}
			workMode = mode_UNCOMPRESS;
			break;

		case '3':
		case '2':
			if (fileVersion == 0)
				fileVersion = c - '0';
			else
				die("Inconsistent file format specifications", EINVAL);
			break;

		case 'B':
			if (blockSizeSpecified)
			{
				die("Invalid multiple definition of block size", EINVAL);
			}
			else
			{
				uint64_t _tmp = getsize(optarg);
				if (_tmp > (1 << 20))
				{
					_tmp = 1 << 20;
					warn("Block size is too big, adjusting to " << _tmp);
				}
				if (_tmp < 512)
				{
					_tmp = 512;
					warn("Block size is too small, adjusting to " << _tmp);
				}
				if (_tmp % 512)
				{
					_tmp -= _tmp % 512;
					warn("Block size is not a multiple of 512, adjusting to " << _tmp);
				}
				blockSize = (size_t) _tmp;
				blockSizeSpecified = true;
			}
			break;

		case 'Z':
			if (strategySpecified )
			{
				die("Invalid multiple definition of strategy", EINVAL);
			}
			strategySpecified = true;
			if (strcmp(optarg, "size") == 0)
				strategy = cs_BESTSIZE;
			else if (strcmp(optarg, "none") == 0)
				strategy = cs_NONE;
			else if (strcmp(optarg, "time") == 0)
				strategy = cs_BESTTIME;
			else if (strcmp(optarg, "smart") == 0)
				strategy = cs_SMART;
			else
				die("Unknown compression strategy " << optarg, EINVAL);
			break;

		case 'L':
			desiredLevel = atoi(optarg);
			if (desiredLevel < 0 || desiredLevel > 9)
				die("Invalid compression level " << optarg, EINVAL);
			allCompressors.setLevel(desiredLevel);
			break;

		case 'C':
			// do not accept '-C' twice
			if (compressorsSpecified)
			{
				die("Invalid multiple definition of usable compression algorithms", EINVAL);
			}
			compressorsSpecified = true;
			allCompressors.setAvailByMask(0);

			// add all specified compressors
			if (strcmp(optarg, "any") == 0)
			{
				allCompressors.setAvailByMask((unsigned int)-1);
			}
			else if (strcmp(optarg, "none") == 0)
				; // do nothing
			else
			{
				if (!allCompressors.isCompressor(optarg))
				{
					die("Unknown compression algorithm " << optarg, EINVAL);
				}
				allCompressors.setAvailByName(optarg);
			}

			// check for further specified compressors, but only up to
			// argc-2 since these must be IN- and OUTFILE
			while (optind < (argc-2) && '-' != argv[optind][0])
			{
				if (!allCompressors.isCompressor(argv[optind]))
					die("Unknown compression algorithm " << argv[optind], EINVAL);
				allCompressors.setAvailByName(argv[optind]);
				if (++optind >= argc)
					break;
			}
			break;

		case 'R':
			if (remoteSpecified)
			{
				die("Invalid multiple definition of remote nodes", EINVAL);
			}
			if (!check_node(optarg, remoteNodes))
				die("Invalid remote node " << optarg << ": address is invalid.", EINVAL);
			// check for further specified nodes
			while (optind < argc && '-' != argv[optind][0])
			{
				if (!check_node(argv[optind], remoteNodes))
					die("Invalid remote node " << argv[optind] << ": address is invalid.", EINVAL);
				++optind;
			}
			remoteSpecified = (remoteNodes.size() != 0);
			break;

		case 'v':
			// be verbose or even very verbose...
			++beVerbose;
			break;

		case 'q':
			beQuiet = true;
			break;

		case 's':
			originalSize = getsize(optarg);
			break;

		case 'j':
			poolSize = getsize(optarg);
			break;

		case 'p':
			defPort = atoi(optarg);
			if (0 <= defPort || defPort > 65535)
				die("Invalid port number " <<  optarg, EINVAL);
			break;

		case 'l':
			workMode |= mode_SERVER;
			break;

		case 'd':
			workMode |= mode_SERVERDETACH;
			break;

		case 't':
			{
			int nMaxThreads = getsize(optarg);
			if (nMaxThreads > 0)
				workThreads = nMaxThreads;
			else
				warn("Bad thread count, using default value " << workThreads);
			}
			break;

		default:
			exit(usage(argv[0]));
		}
	}

	// check for sensible options

	if (mode_SERVERDETACH == (workMode & (mode_SERVER | mode_SERVERDETACH)))
	{
		die("-d can only be specified with option -l", EINVAL);
	}
	if (workMode & mode_SERVER)
	{
		// check some more useless settigs
		if (blockSizeSpecified)
			die("Block size (-B) cannot be specified in server mode", EINVAL);
		if (fileVersion != 0)
			die("File version cannot be specified in server mode", EINVAL);
		if (originalSize)
			die("-s is useless in server mode", EINVAL);
		if (strategySpecified)
			die("Strategy (-Z) is useless in server mode", EINVAL);
		if (poolSize)
			die("Pool size (-j) is useless in server mode", EINVAL);
		if (compressorsSpecified)
			die("-C is useless in server mode", EINVAL);

		// if we're happy with all settings - start the server!
		check_warn_processors();
		return start_server();
	}
	else if ( mode_UNCOMPRESS == workMode)
	{
		// check some more useless settings
		if (blockSizeSpecified)
			die("Block size (-B) cannot be specified when uncompressing", EINVAL);
		if (fileVersion != 0)
			die("File version cannot be specified when uncompressing", EINVAL);
		if (originalSize)
			die("-s is useless when uncompressing", EINVAL);
		if (strategySpecified)
			die("Strategy (-Z) is useless when uncompressing", EINVAL);
		if (compressorsSpecified)
			die("-C is useless when uncompressing", EINVAL);
	}
	else if (mode_SUSPEND == workMode)
	{
		if (optind > argc-1)
		{
			usage(argv[0]);
			die("Device must be specified", EINVAL);
		}
		return do_suspend(argv[optind]);
	}

	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// local operation non-server mode from here
	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	if (optind > argc - 2)
	{
		usage(argv[0]);
		die("Infile and outfile must be specified", EINVAL);
	}

	// current offset of compressed data block (used for creation v2 or v3 files)
	loff_t curr_block_offset=0;

	// check poolsize
	if (poolSize <=0 ) poolSize = workThreads+3;

	const char *fromfile = argv[optind], *tofile = argv[optind + 1];
	int in = -1;          // fd from input
	FILE *outfh = NULL;   // FILE for output

	// check in / out FILEs
	if (strcmp(tofile, "-"))
	{
		// not STDOUT, try open
		outfh = fopen(tofile, "w+");
		if (!outfh)
			die("Opening output file for writing", EBADF);
	}
	else
	{
		// STDOUT as output ? only supported for creating v3+ Files
		// uncompression is never a problem
		if (fileVersion <= 2 && mode_COMPRESS == workMode)
			die("Unrewindable output, cannot produce cloop v2 files on STDOUT.", EBADF);
		outfh = stdout;
	}

	if (strcmp(fromfile, "-"))
	{
		// not STDIN, check size of that file
		struct stat buf;
		if (!originalSize)
		{
			stat(fromfile, &buf);
			originalSize = buf.st_size;
		}
		in = ::open(fromfile, O_RDONLY | O_LARGEFILE);
		if (in > 0)
		{
			posix_fadvise(in, 0, 0, POSIX_FADV_NOREUSE | POSIX_FADV_SEQUENTIAL | POSIX_FADV_DONTNEED);
		}
	}
	else if (mode_UNCOMPRESS == workMode)
	{
		die("Cannot uncompress from STDIN.", EBADF);
	}
	else
	{
		// STDIN as input
		if (2 == fileVersion && 0 == originalSize)
			die("Cannot produce v2 cloop files without knowing its total uncompressed size: provide it with -s", EINVAL);
		in = fileno(stdin);
	}

	if (in < 0)
		die("Opening input", EBADF);

	if (mode_COMPRESS == workMode)
	{
		// initializing and normalizing parameters
		if (fileVersion == 0)
			fileVersion = 3;

		// select some compressors (if not set) based on the strategy and file version
		if (2 == fileVersion )
		{
			// version 2 only supports zlib and 7zip - since we drooped 7zip it can only be
			// zlib :)
			if (allCompressors.isAnyAvailable(CLOOP3_COMPRESSOR_LZ4|CLOOP3_COMPRESSOR_LZO1X|CLOOP3_COMPRESSOR_XZ))
			{
				if (compressorsSpecified)
					warn("Cannot do a v2 file with selected compressors (" << allCompressors.list_compressors() <<"). Using default 7zip and zlib.");
				allCompressors.setAvailByMask(CLOOP3_COMPRESSOR_ZLIB);
			}
			if (cs_NONE == strategy)
			{
				strategy = cs_BESTSIZE;
				allCompressors.setAvailByMask(CLOOP3_COMPRESSOR_ZLIB);
				allCompressors.setLevel(0);
				warn("Cannot do a v2 file without compression, using zlib and level 0 compression instead.");
			}
			else if (cs_BESTTIME == strategy)
			{
				strategy = cs_BESTSIZE;
				allCompressors.setAvailByMask(CLOOP3_COMPRESSOR_ZLIB);
				allCompressors.setLevel(5);
				warn("Cannot select fastest algorithm in a v2 file using zlib and level 5 compression instead.");
			}
			else if (cs_BESTSIZE == strategy)
			{
				allCompressors.setAvailByMask(CLOOP3_COMPRESSOR_ZLIB);
				allCompressors.setLevel(9);
				info("For smallest v2 files zlib level 9 compression will be used.");
			}
			else if (cs_SMART == strategy)
			{
				allCompressors.setAvailByMask(CLOOP3_COMPRESSOR_ZLIB);
				allCompressors.setLevel(5);
				info("For fastest v2 files zlib level 5 compression will be used.");
			}
		}
		else if (fileVersion >= 3)
		{
			if (cs_BESTTIME == strategy && !compressorsSpecified)
			{
				// if fastest decompression speed is desired, usually lz4 and lzo win the race over all
				// other compressors.
				allCompressors.setAvailByMask(CLOOP3_COMPRESSOR_LZ4|CLOOP3_COMPRESSOR_LZO1X);
			}
			if (cs_BESTSIZE== strategy  && !compressorsSpecified)
			{
				// if best compression is desired xz beats them all
				allCompressors.setAvailByMask(CLOOP3_COMPRESSOR_XZ);
			}
			if (strategy == cs_SMART && !compressorsSpecified)
			{
				// if smart compression is desired again lzo and lz4 provide the best
				// size*time weight
				allCompressors.setAvailByMask(CLOOP3_COMPRESSOR_LZO1X|CLOOP3_COMPRESSOR_LZ4);
			}
		}

		// check block size and set default if needed
		if (blockSize <= 0)
			blockSize = getsize(defaultBlockSize);

		// pre-calculate some values and do some checks
		numBlocks = originalSize / blockSize;
		if (originalSize % blockSize)
			++numBlocks;
		if (numBlocks == 0 && fileVersion <= 2)
			die("Cannot produce cloop v2 file - unknown block count", EBADF);
		if (numBlocks * blockSize < originalSize)
			die("Cannot produce more than " << 0xffffffffUL << " compressed blocks - increase block size!", EINVAL);
		if (numBlocks)
		{
			lengths.resize(numBlocks, 0);
			flags.resize(numBlocks, 0);
		}

		info("Block size "<< blockSize << ", number of compressed blocks: " << numBlocks);
		info("Compression strategy: " << (strategy == cs_BESTTIME ? "fast decompression" : (strategy == cs_BESTSIZE ? "tiny filesize" : (strategy == cs_SMART ? "smart" : "store only")))
					<< (compressorsSpecified ? " using " : ". No compressors specified, using ")
					<< allCompressors.list_compressors());

		if (fileVersion == 2)
		{
			/* Write the v2 head... */
			char buf[CLOOP2_HEADERSIZE] = { 0, };
			memcpy(buf, CLOOP_PREAMBLE, sizeof(CLOOP_PREAMBLE));
			*(int*) (&buf[CLOOP2_HEADERSIZE-8]) = htonl(blockSize);
			*(int*) (&buf[CLOOP2_HEADERSIZE-4]) = htonl(numBlocks);
			// header is @ 0 bytes offset, seek back
			fseeko(outfh, 0, SEEK_SET);
			fwrite(buf, CLOOP2_HEADERSIZE, 1, outfh);
			// set file position to first compressed block, i.e.
			// skip (num_blocks+1) * 8 bytes offsets
			fseeko(outfh, (numBlocks + 1) * sizeof(loff_t), SEEK_CUR);
			curr_block_offset = ftello(outfh);
		}
		else if (fileVersion >= 3)
		{
			// nothing to do here, since all index data is written at the end of the file
			// this will be done after the compresion has finished
		}
	}
	else if (mode_UNCOMPRESS == workMode)
	{
		// check if INPUT is really a cloop file
	}

	//
	// NOW: ALL is sorted out - we can start working
	//
	check_warn_processors();

	// Create all needed threads
	LocalFileInputThread inpth(in);
	LocalFileOutputThread outpth(outfh);

	// if we have specified some remote nodes, we start separate threads for them
	if (remoteNodes.size())
	{
		// but only if it does make sense
		if (mode_COMPRESS == workMode && (0 == allCompressors.availableMask() || cs_NONE == strategy))
			warn("Remote nodes specified but compression strategy is 'none' - skipping!");
		else
		{
			new NetDiscoverReplyThread;
			new NetDiscoverUDPThread;
		}
	}

	// GO, GO, GO
	for (uint32_t i = 0; i < workThreads; ++i)
	{
		new CompressThread();
	}

	// wait for all output to be written
	outpth.join();

	// signal all other threads to end
	allThreads.cancel(true);

	// we don't need that anymore
	close(in);

	if (mode_COMPRESS == workMode)
	{

		// remember file position at which index table starts (v3)
		loff_t index_table = ftello(outfh);

		// two temps for htonl* conversion
		uint32_t t32;
		uint64_t t64;

		// write index data into cloop file
		if (fileVersion == 2)
		{
			// Write compressed block offsets
			// rewind file
			fseeko(outfh, CLOOP2_HEADERSIZE, SEEK_SET);
			DEBUG(2, "v2: Rewinding to file pos " << CLOOP2_HEADERSIZE);
		}
		else if (fileVersion == 3)
		{
			// check if we are on a 4096 byte boundary
			if (index_table % 4096)
			{
				index_table += (4096 - (index_table % 4096));
				fseeko(outfh, index_table, SEEK_SET);
			}
			DEBUG(2, "v3: Index Table numBlocks @ " << index_table);
			// Number of blocks
			t32 = htonl(numBlocks);
			fwrite(&t32, sizeof(t32), 1, outfh);
			// Block size
			DEBUG(2, "v3: Index Table blockSize @ " << ftello(outfh));
			t32 = htonl(blockSize);
			fwrite(&t32, sizeof(t32), 1, outfh);
			// original file size (uncompressed)
			DEBUG(2, "v3: Index Table originalSize @ " << ftello(outfh));
			t64 = htonll(originalSize);
			fwrite(&t64, sizeof(t64), 1, outfh);
			// offset of first compressed block is zero
			curr_block_offset = 0;
		}

		info("Writing index for " << numBlocks << " block(s)...");

		/* Write offsets n offsets (v2 & v3), finally total compressed length, then flags (v3) */
		for (size_t i = 0; i < numBlocks; ++i)
		{
			//DEBUG("Index " << i << ": " << curr_block_offset);
			if (i%50 == 0 ) DEBUG(3, "offset[" << i << "]=" << curr_block_offset << " @ " << ftello(outfh));
			t64 = htonll(curr_block_offset);
			fwrite(&t64, sizeof(t64), 1, outfh);
			curr_block_offset += lengths[i];
		}

		t64 = htonll(curr_block_offset);
		fwrite(&t64, sizeof(t64), 1, outfh);

		if (fileVersion >= 3)
		{
			// Write flags now
			for (size_t i = 0; i < numBlocks; ++i)
			{
				t32 = htonl(flags[i]);

				if (i%50 == 0 ) DEBUG(3, "flag[" << i << "]=" << t32 << " @ " << ftello(outfh));

				fwrite(&t32, 1, sizeof(t32), outfh);
			}
			// write table index now
			DEBUG(2, "v3: Index Table start offset stored @ " << ftello(outfh) << " contains " << index_table);
			t64 = htonll(index_table);
			fwrite(&t64, sizeof(t64), 1, outfh);
			// file version
			DEBUG(2, "v3: Index Table fileVersion @ " << ftello(outfh));
			t32 = htonl(fileVersion);
			fwrite(&t32, sizeof(t32), 1, outfh);
			// signature finally
			DEBUG(2, "v3: Index Table signature @ " << ftello(outfh));
			fwrite(CLOOP3_SIGNATURE, CLOOP3_SIGNATURE_LEN, 1, outfh);
		}
		fflush(outfh);
		fclose(outfh);

		if (!beQuiet)
		{
			// final statistics
			fprintf(stderr, "\nTotal cloop file statistics:\n"
					"Method        Usage  Uncompressed Compressed Ratio\n"
					"---------------------------------------------------\n");
			//for (compressor_map::iterator x = Compressors.begin(); x != Compressors.end(); x++)
			uint32_t uncompressed = numBlocks;
			uint64_t compressed_size=0;
			for (auto &x : allCompressors)
			{
				compressorInfo_t &c = *x.second;
				uncompressed -= c.useCounter;
				fprintf(stderr, "%6s %4d [", c.name, c.useCounter);
				if (0 == c.useCounter) fprintf(stderr, "   0%%");
				else if (c.useCounter==numBlocks) fprintf(stderr, " 100%%");
				else fprintf(stderr, "%4.1f%%",100.0F*double(c.useCounter)/numBlocks);
				fprintf(stderr, "]  %12d %10d ", c.useCounter*blockSize, c.usedBytes);
				compressed_size += c.usedBytes;
				if (c.useCounter) fprintf(stderr, "%4.1f%%\n", 100.0F*double(c.usedBytes) / (c.useCounter*blockSize) );
				else fprintf(stderr, "  -  \n");
			}
			if (uncompressed)
			{
				if (uncompressed == numBlocks)
					fprintf(stderr, "  none %4d [ 100%%]  %12d %10d  100%%\n", uncompressed, uncompressed*blockSize, uncompressed*blockSize);
				else
					fprintf(stderr, "  none %4d [%4.1f%%]  %12d %10d  100%%\n", uncompressed,
							100.0F * double(uncompressed)/numBlocks, uncompressed*blockSize, uncompressed*blockSize);
			}
			fprintf(stderr, "---------------------------------------------------\n"
							"total %5u [ 100%%]  %12llu %10llu ",
							numBlocks, originalSize, compressed_size+uncompressed*blockSize);
			double ratio = 100.0F*(double(compressed_size)+double(uncompressed)*blockSize) / originalSize;

			if (ratio > 99.94F) fprintf(stderr, " 100%%\n");
			else fprintf(stderr, "%4.1f%%\n", ratio);

		}
	}
	return 0;
}
