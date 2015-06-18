/*
 * debug.h
 *
 *  Created on: 09.12.2013
 *      Author: dplasa
 */

#ifndef DEBUG_H_
#define DEBUG_H_

#include <iostream>

extern int beVerbose;
extern bool beQuiet;

//#define DEBUG(level, x)
#define DEBUG(level, x) do { if (level <= beVerbose) std::cerr << x << std::endl; } while (0)

#define die(msg, errcode) do { std::cerr << "ERROR: " << msg << std::endl;  fflush(stderr); exit(errcode); } while(0)
#define warn(msg) do { std::cerr << "WARNING: " << msg <<std::endl;  fflush(stderr); } while(0)
#define info(msg) do { if (!beQuiet) std::cerr << "INFO: " << msg <<std::endl;  } while(0)

#endif /* DEBUG_H_ */
