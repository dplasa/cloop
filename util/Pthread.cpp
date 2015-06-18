/*
 * Pthread.cpp
 *
 *  Created on: 04.12.2013
 *      Author: dplasa
 */

#include <unistd.h>
#include "Pthread.h"
using namespace std;

int Pthread::num_id = 0;
Plockable Pthread::_global_lock;
Pconditional Pthread::_global_condition;

PthreadRegister allThreads;

Pthread::Pthread(const char* name, bool dostart)
{
  _id = ++num_id;
  _name = name;
  _state = pts_STOP;
  thread_id = 0;
  // register in allThreads
  allThreads.push_back(this);
  // start if wanted
  if (dostart)
	  start();
}

// pthread wrapper body function that calls Pthread::loop()
void * threadfn(void* arg)
{
   Pthread * t = (Pthread *) arg;
   for (;!(t->_state & Pthread::pts_TERMINATE);)
   {
	   pthread_testcancel();
	   if (t->_state & Pthread::pts_RUN)
		   t->loop();
	   else
		   usleep(1000);
   }
   //tDEBUG("softly exiting...");
   t->_state = Pthread::pts_STOP;
   return NULL;
}
