/*
 * ebrunner.h
 *
 *  Created on: 31 янв. 2021 г.
 *      Author: ed
 */

#ifndef INCLUDE_EBRUNNER_H_
#define INCLUDE_EBRUNNER_H_

#include "config.h"

extern "C"
{
struct event_base;
}

namespace acng
{
class dlcon;
class IDlConFactory;

// tool helper class for acngtool and httpfs, runs event and download threads
class ACNG_API evabaseFreeRunner
{
	class Impl;
public:
	evabaseFreeRunner(const IDlConFactory &pDlconFac, bool withDownloader, unsigned killTimeout = 0);
	~evabaseFreeRunner();
	dlcon& getDownloader();
	event_base* getBase();
private:
	Impl *m_pImpl;
};

}


#endif /* INCLUDE_EBRUNNER_H_ */
