#ifndef _COMPRESSION_HELPERS_H_
#define _COMPRESSION_HELPERS_H_

#include <list>
#include <map>
#include <time.h>
#define delta_ts(x,y) (x.tv_sec-y.tv_sec)*1000000000+x.tv_nsec-y.tv_nsec
#include "debug.h"
#include <errno.h>
#include <string.h>
#include "cloop.h"

// system zlib for gzip / gunzip style compression
// also used to uncompress 7z compressed blocks
#include <zlib.h>

// ligntning fast compressor, an edge faster than lzo
#include <lz4hc.h>
#include <lz4.h>

// lzo1x compressor as used in the kernel
#include <lzo/lzo1x.h>

// xz compression
#include <lzma.h>

enum compressionStrategy_t
{
	// do not compress at all, just store all data uncompressed
	cs_NONE = 0,
	// optimize cloop file for smallest size
	cs_BESTSIZE,
	// optimize cloop file for fastest decompression time
	cs_BESTTIME,
	// trade speed for size... i.e. optimize for decompression time * size to be minimal
	cs_SMART
};

// information structure for any compression/decompression algorithm
enum compressorResult_t
{
	C_OK = 0,
	C_INCOMPRESSIBLE,
	C_ERROR
};

struct compressor_t
{
	friend struct compressor_map;
public:
	virtual ~compressor_t() {}

	// must be overloaded in derived class, does the real compression work
	virtual compressorResult_t compress(const void *dest, size_t &destlen, const void* src,
			size_t srclen, int level) = 0;

	// must be overloaded in derived class, does the real uncompression work
	virtual compressorResult_t uncompress(void *dest, size_t &destlen, const void* src,
			size_t srclen) = 0;

	// worst size of compression result
	virtual size_t worstsize(size_t srclen) = 0;
};

// the normal gzip
struct compressor_zlib: public compressor_t
{
	virtual compressorResult_t compress(const void *dest, size_t &destlen, const void* src,
			size_t srclen, int _level)
	{
		long unsigned int tmp = destlen;
		int res = compress2((unsigned char*) dest, &tmp,
				(const unsigned char*) src, srclen, _level);
		if (res == Z_OK)
		{
			destlen = tmp;
			return C_OK;
		}
		return C_INCOMPRESSIBLE;
	}

	virtual compressorResult_t uncompress(void *dest, size_t &destlen, const void* src,
			size_t srclen)
	{
		long unsigned int tmp = destlen;
		int res = ::uncompress((unsigned char*) dest, &tmp,
				(const unsigned char*) src, srclen);
		if (res == Z_OK)
		{
			destlen = tmp;
			return C_OK;
		}
		return C_ERROR;
	}
	size_t worstsize(size_t srclen)
	{
		return (size_t) compressBound((unsigned) srclen);
	}
};

// wrapper for lz4
struct compressor_lz4: public compressor_t
{
	virtual compressorResult_t compress(const void *dest, size_t &destlen, const void* src,
			size_t srclen, int _level)
	{
		int res = LZ4_compressHC_limitedOutput((const char*) src, (char*) dest,
				(int) srclen, (int) destlen);
		if (res <= 0)
			return C_INCOMPRESSIBLE;
		destlen = (size_t) res;
		return C_OK;
	}

	virtual compressorResult_t uncompress(void *dest, size_t &destlen, const void* src,
			size_t srclen)
	{
		int res = LZ4_decompress_fast((const char*) src, (char*) dest,
				(int) destlen);
		if (res <= 0) return C_ERROR;
		return C_OK;
	}
	size_t worstsize(size_t srclen)
	{
		// use safe lz4 compression function that will stop if output would
		// exceed input size
		return srclen;
	}
};

// wrapper for lzo1x
struct compressor_lzo1x: public compressor_t
{
public:
	compressor_lzo1x()
	{
		if (lzo_init() != LZO_E_OK)
			die("internal error - lzo_init() failed !!!", -EINVAL);
	}
	virtual ~compressor_lzo1x() {}

	virtual compressorResult_t compress(const void *dest, size_t &destlen, const void* src,
			size_t srclen, int _level)
	{
		unsigned char workmem[LZO1X_1_15_MEM_COMPRESS];
		long unsigned int tmp = destlen;
		int res = lzo1x_1_15_compress((const unsigned char*)src, srclen, (unsigned char*) dest, &tmp, &workmem);
		destlen = tmp;
		return LZO_E_OK==res ? C_OK : C_INCOMPRESSIBLE;
	}

	virtual compressorResult_t uncompress(void *dest, size_t &destlen, const void* src,
			size_t srclen)
	{
		long unsigned int tmp = destlen;
		int res = lzo1x_decompress_safe((const unsigned char*) src, srclen, (unsigned char*)dest, &tmp, NULL);
		destlen = tmp;
		return res>= 0 ? C_OK : C_ERROR;
	}

	size_t worstsize(size_t srclen)
	{
		// use safe lzo compression function that will stop if output would
		// exceed input size
		#define lzo1x_worst_compress(x) ((x) + ((x) / 16) + 64 + 3)
		return lzo1x_worst_compress(srclen);
	}
};

// wrapper for xz/lzma
struct compressor_xz: public compressor_t
{
public:
	virtual ~compressor_xz()
	{
	}

	virtual compressorResult_t compress(const void *dest, size_t &destlen, const void* src,
			size_t srclen, int)
	{
		lzma_stream strm;
		memset(&strm, 0, sizeof(strm));

		// Initialize the encoder using a preset (level+flag). We set the integrity to
		// none (CRC64 is the default in the xz command line tool).
		lzma_ret ret = lzma_easy_encoder(&strm, 9 | LZMA_PRESET_EXTREME , LZMA_CHECK_NONE);
		if (ret != LZMA_OK)
		{
//			const char *msg;
//			switch (ret) {
//			case LZMA_MEM_ERROR:
//				msg = "Memory allocation failed";
//				break;
//
//			case LZMA_OPTIONS_ERROR:
//				msg = "Specified preset is not supported";
//				break;
//
//			case LZMA_UNSUPPORTED_CHECK:
//				msg = "Specified integrity check is not supported";
//				break;
//
//			default:
//				// This is most likely LZMA_PROG_ERROR indicating a bug in
//				// this program or in liblzma. It is inconvenient to have a
//				// separate error message for errors that should be impossible
//				// to occur, but knowing the error code is important for
//				// debugging. That's why it is good to print the error code
//				// at least when there is no good error message to show.
//				msg = "Unknown error, possibly a bug";
//				break;
//			}
//			DEBUG(0, "xz compress error: " << msg );
			return C_ERROR;
		}

		// This tells lzma_code() when there will be no more input.
		lzma_action action = LZMA_FINISH;

		// Initialize the input and output pointers.
		strm.next_in = (const unsigned char*)src;
		strm.avail_in = srclen;
		strm.next_out = (unsigned char*)dest;
		strm.avail_out = destlen;

		// Tell liblzma do the actual encoding.
		//
		// This reads up to strm->avail_in bytes of input starting
		// from strm->next_in. avail_in will be decremented and
		// next_in incremented by an equal amount to match the
		// number of input bytes consumed.
		//
		// Up to strm->avail_out bytes of compressed output will be
		// written starting from strm->next_out. avail_out and next_out
		// will be incremented by an equal amount to match the number
		// of output bytes written.

		ret = lzma_code(&strm, action);
		lzma_end(&strm);

		// If the output buffer is full or if the compression finished
		// successfully, write the data from the output bufffer to
		// the output file.
		if (strm.avail_out == 0 || ret == LZMA_STREAM_END )//|| ret == LZMA_OK)
		{
			destlen = srclen - strm.avail_out;
			return C_OK;
		}
		return C_INCOMPRESSIBLE;
	}

	virtual compressorResult_t uncompress(void *dest, size_t &destlen, const void* src,
			size_t srclen)
	{
		lzma_stream strm;
		memset(&strm, 0, sizeof(strm));
		lzma_ret ret = lzma_stream_decoder(&strm, UINT64_MAX, 0);
		if (ret != LZMA_OK)
		{
			const char *msg;
			switch (ret) {
			case LZMA_MEM_ERROR:
				msg = "Memory allocation failed";
				break;

			case LZMA_OPTIONS_ERROR:
				msg = "Unsupported decompressor flags";
				break;

			default:
				// This is most likely LZMA_PROG_ERROR indicating a bug in
				// this program or in liblzma. It is inconvenient to have a
				// separate error message for errors that should be impossible
				// to occur, but knowing the error code is important for
				// debugging. That's why it is good to print the error code
				// at least when there is no good error message to show.
				msg = "Unknown error, possibly a bug";
				break;
			}
			DEBUG(3, "xz compress failed: " << msg);
			return C_ERROR;
		}
		strm.avail_in = srclen;
		strm.next_in = (const unsigned char*)src;
		strm.next_out = (unsigned char*)dest;
		strm.avail_out = destlen;
		ret = lzma_code(&strm, LZMA_FINISH);
		lzma_end(&strm);

		if (strm.avail_out == 0 || ret == LZMA_STREAM_END)
		{
			destlen -= strm.avail_out;
			return C_OK;
		}
		return C_ERROR;
	}

	size_t worstsize(size_t srclen)
	{
		// xz compression function will stop if output would
		// exceed input size
		return srclen;
	}
};

struct compressionResult_t
{
	compressionResult_t()
	{
		buf = NULL;
		bufLen = 0;
		compressedSize = 0;
		cTime = 0;
		uTime = 0;
		cid = 0;
		res = C_OK;
	}
	~compressionResult_t()
	{
		discard();
	}
	void discard()
	{
		if (buf) { free(buf); buf = NULL; }
		cid = 0;
		res = C_ERROR;
	}
	const void * move_buffer()
	{
		const void * tmp = buf;
		buf = NULL;
		return tmp;
	}
	void* buf;			    // buffer to hold compression result
	size_t bufLen;			// length of buffer
	size_t compressedSize;  // length of data in buffer
	uint64_t cTime, uTime;// time in ns that compression / decompression took
	int cid;				// ID of compressor
	compressorResult_t res;	// result of the compression
	static bool compare_size(const compressionResult_t& first, const compressionResult_t &second)
	{
		if (first.res != C_OK) return false;
		if (second.res != C_OK) return true;
		return first.compressedSize < second.compressedSize;
	}
	static bool compare_time(const compressionResult_t& first, const compressionResult_t &second)
	{
		if (first.res != C_OK) return false;
		if (second.res != C_OK) return true;
		return first.uTime < second.uTime;
	}
	static bool compare_smart(const compressionResult_t& first, const compressionResult_t &second)
	{
		if (first.res != C_OK) return false;
		if (second.res != C_OK) return true;
		return first.uTime*first.compressedSize < second.uTime*second.compressedSize;
	}
};


struct compressorInfo_t
{
	unsigned int useCounter;	// statistics - how many times used during creation of a cloop file
	size_t usedBytes;			// statistics - how many bytes compressed data by this compressor?
	bool available;				// can be used to compress data
	int level;					// hint desired compression level (zlib only)
	const char *name;			// name of the compressor algorithm
	compressor_t* c;			// the compressor class doing the work

	compressorInfo_t(const char* _name, compressor_t* _c) :
		useCounter(0), usedBytes(0), available(true), level(2), name(_name), c(_c) {}
	virtual ~compressorInfo_t()
	{
		delete c; c = NULL;
	}
};


// a map with all known compressor
typedef std::map<unsigned int, compressorInfo_t*> compressorMap_pt;
struct compressorMap_t: public compressorMap_pt
{
	compressorMap_t()
	{
		// lz4
		compressorInfo_t *lz4info = new compressorInfo_t("lz4", new compressor_lz4());
		std::pair<int, compressorInfo_t*> lz4(CLOOP3_COMPRESSOR_LZ4, lz4info);
		this->insert(lz4);

		// zlib
		compressorInfo_t *zlibinfo = new compressorInfo_t("zlib", new compressor_zlib());
		std::pair<int, compressorInfo_t*> zlib(CLOOP3_COMPRESSOR_ZLIB,	zlibinfo);
		this->insert(zlib);

		// lzo (1x)
		compressorInfo_t *lzo1xinfo = new compressorInfo_t ("lzo", new compressor_lzo1x());
		std::pair<int, compressorInfo_t*> lzo1x(CLOOP3_COMPRESSOR_LZO1X, lzo1xinfo);
		this->insert(lzo1x);

		// xz
		compressorInfo_t *xzinfo = new compressorInfo_t ("xz", new compressor_xz());
		std::pair<int, compressorInfo_t*> xz(CLOOP3_COMPRESSOR_XZ, xzinfo);
		this->insert(xz);
	}

	std::string list_compressors(char seperator=' ', bool all=false)
	{
		std::string tmp;
		for (auto &i: *this)
		{
			if (!(all || i.second->available)) continue;
			tmp += i.second->name;
			tmp += seperator;
		}
		if (tmp.length()) tmp.resize(tmp.length()-1);
		return tmp;
	}

	const char* name(int cid)
	{
		if (find(cid) == end()) return "(unknown)";
		return find(cid)->second->name;
	}

	int countAvailable()
	{
		int tmp=0;
		for (auto &x: *this)
			if (x.second->available) ++tmp;
		return tmp;
	}

	bool isCompressor(const char *name)
	{
		for (auto &i: *this)
			if (strcmp(i.second->name, name) == 0)
				return true;
		return false;
	}

	void setAvailByMask(unsigned int avail_mask)
	{
		for (auto &i: *this)
			i.second->available = ((avail_mask & i.first) == i.first);
	}

	void setAvailByName(const char *name)
	{
		for (auto &i: *this)
			if (strcmp(i.second->name, name) == 0)
			{
				i.second->available = true;
				break;
			}
	}

	void setLevel(int level)
	{
		for (auto &i: *this)
			i.second->level = level;
	}

	void incUsecounter(const compressionResult_t& res)
	{
		compressorMap_t::iterator i = find(res.cid);
		if (end() != i)
		{
			compressorInfo_t *ci = i->second;
			++ci->useCounter;
			ci->usedBytes += res.compressedSize;
		}
	}

	bool isAvailable(unsigned int idmask)
	{
		unsigned int tmp = availableMask();
		return (tmp & idmask) == idmask;
	}

	bool isAnyAvailable(unsigned int idmask)
	{
		unsigned int tmp = availableMask();
		return (tmp & idmask);
	}

	unsigned int availableMask(bool all = false)
	{
		unsigned int tmp=0;
		for (auto &i: *this)
		{
			if (!(all || i.second->available)) continue;
			tmp |=i.first;
		}
		return tmp;
	}

	void compressAll(std::list<compressionResult_t> & dest, const void * src, size_t srclen, bool timeuncompress=false)
	{
		void *tmpbuf = timeuncompress ? malloc(srclen) : NULL;
		size_t tmplen = srclen;
		for (auto &i: *this)
		//for (compressor_map_t::iterator i = begin(); i != end(); i++)
		{
			if (!i.second->available) continue;
			dest.emplace_back();
			compressionResult_t &tmp = dest.back();
			tmp.cid = i.first;
			//DEBUG("Compressing " << srclen << " bytes with " << i.second->name());
			compress(tmp, src, srclen, *i.second);
			//DEBUG("Result is " << tmp.res << " size now " << tmp.compressed_len << "bytes, " << tmp.c_nsec << " ns");
			if (C_ERROR== tmp.res)
				die("Failed to compress " << srclen << " bytes using " << i.second->name, -EBADMSG);
			else if (C_INCOMPRESSIBLE == tmp.res)
				;//DEBUG(srclen << " bytes incompressible for " << i.second->name());
			else if (C_OK == tmp.res && timeuncompress)
			{
				uncompress(tmpbuf, tmplen, tmp, *i.second);
				if (memcmp(tmpbuf, src, srclen))
					die("Uncompressed data mismatch using " << i.second->name,-EBADMSG);
			}
			if (C_ERROR== tmp.res || tmplen !=srclen)
				die("Failed to uncompress " << tmp.compressedSize << " bytes using " << i.second->name,-EBADMSG);
			if (C_OK != tmp.res)
			{
				//DEBUG("Won't be using compressor " << i.second->name() << " this time.");
				dest.erase(dest.begin());
			}
		}
		free(tmpbuf);
	}

	compressorResult_t uncompress(unsigned int compressor, void * dest, size_t destlen, const void * src, size_t srclen)
	{
		compressorMap_pt::iterator i = find(compressor);
		if (end() == i)
			return C_ERROR;
		compressionResult_t tmp;
		tmp.buf = (void*)src;
		tmp.compressedSize = srclen;
		uncompress(dest, destlen, tmp, *i->second);
		return tmp.res;
	}
private:

	// compress a piece of data
	// IN parameters: 	compression_result with a buffer with destlen bytes (which should be at least
	//					worstsize(srclen) bytes) or a buffer NULL which causes compress to malloc the
	//					needed space by itself
	//					src points to memory of srclen bytes
	void compress(compressionResult_t& res, const void* src, size_t srclen, compressorInfo_t &ci)
	{
		// allocate buffer if needed
		if (res.buf == NULL)
		{
			res.bufLen = ci.c->worstsize(srclen);
			res.buf = malloc(res.bufLen);
		}
		timespec ts1, ts2;
		// call the real compressor
		size_t destsize = res.bufLen;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts1);
		res.res = ci.c->compress(res.buf, destsize, src, srclen, ci.level);
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts2);
		res.compressedSize = destsize;
		if (destsize >= srclen) res.res = C_INCOMPRESSIBLE;
		res.cTime = delta_ts(ts2, ts1);
	}

	// uncompress
	// IN parameters: src points to memory of srclen bytes, dest points to a buffer of destlen bytes
	// OUT parameters: destlen will be the size of uncompressed data (if returns successful)
	void uncompress(void *dest, size_t &destlen, compressionResult_t& res, compressorInfo_t &ci)
	{
		// get requested compressor

		timespec ts1, ts2;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts1);
		res.res = ci.c->uncompress(dest, destlen, res.buf, res.compressedSize);
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts2);
		res.uTime = delta_ts(ts2, ts1);
	}
};

// map with all available compressors
extern compressorMap_t allCompressors;

#endif // _COMPRESSION_HELPERS_H_
