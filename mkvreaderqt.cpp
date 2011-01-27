// Copyright (c) 2010 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

#include "mkvreaderqt.hpp"

extern "C" {
#include "log.h"
}


//--------------------------------------------------------------------------------
MkvReaderQT::MkvReaderQT() :
  m_length(0), m_dataRef(NULL), m_dataHandler(NULL)
{
}

//--------------------------------------------------------------------------------
MkvReaderQT::~MkvReaderQT()
{
  Close();
}

//--------------------------------------------------------------------------------
int MkvReaderQT::Open(Handle dataRef, OSType dataRefType)
{
  if (dataRef == NULL)
    return -11;

  if ((m_dataRef) || (m_dataHandler))
    return -2;

  m_dataRef = dataRef;

  long fileSize = 0;

  // Retrieve the best data handler component to use with the given data reference, for read purpoases.
	// Then open the returned component using standard Component Manager calls.
  OSType err;
  ComponentInstance dataHandler = 0;
	err = OpenAComponent(GetDataHandler(dataRef, dataRefType, kDataHCanRead), &dataHandler);
	if (err) return -3;

  // Provide a data reference to the data handler.
	// Then you may start reading and/or writing
	// movie data from that data reference.
	err = DataHSetDataRef(dataHandler, dataRef);
	if (err) return -4;

  // Open a read path to the current data reference.
  // You need to do this before your component can read data using a data handler component.
	err = DataHOpenForRead(dataHandler);
	if (err) return -5;

  // Get the size, in bytes, of the current data reference.
  // This is functionally equivalent to the File Manager's GetEOF function.
	err = DataHGetFileSize(dataHandler, &fileSize);
	if (err) return -6;
  m_length = fileSize;

  // JAK
#if 0
  Boolean buffersReads;
  Boolean buffersWrites;
  DataHDoesBuffer(dataHandler,  &buffersReads, &buffersWrites);
  dbg_printf("DataHDoesBuffer() buffersReads = %d\n", buffersReads);
  long blockSize = 0;
  DataHGetPreferredBlockSize(dataHandler, &blockSize);
  dbg_printf("DataHGetPreferredBlockSize() is %ld\n", blockSize);
#endif

  // dbg_printf("[WebM Import] DataHGetFileSize = %d\n", fileSize);
  //  dbg_printf("[WebM Import] sizeof Header %d\n", sizeof(header));

  m_dataHandler = dataHandler;
  return 0;
}

//--------------------------------------------------------------------------------
void MkvReaderQT::Close()
{
  // ****
}

//--------------------------------------------------------------------------------
int MkvReaderQT::Read(long long position, long length, unsigned char* buffer)
{
  // sanity checks
  if ((m_dataRef == NULL) || (position < 0) || (length < 0) || (position >= m_length))
    return -1;

  if (length == 0)
    return 0;

  if (length != 1)
    dbg_printf("MkvReaderQT::Read() len = %ld\n", length);

  // DatHGetPreferredBlockSize()
  // MovieImportSetIdleManager(store, store->idleManager)

  // seek and read
	// This function provides both a synchronous and an asynchronous read interface. Synchronous read operations
	// work like the DataHGetData function--the data handler component returns control to the client program only
	// after it has serviced the read request. Asynchronous read operations allow client programs to schedule read
	// requests in the context of a specified QuickTime time base. The DataHandler queues the request and immediately
	// returns control to the caller. After the component actually reads the data, it calls the completion function -
	// calling of the completion will occurs in DataHTask. Not all data handlers support scheduling, if they don't
	// they'll complete synchronously and then call the completion function. Additionally as a note, some DataHandlers
	// support 64-bit file offsets, for example DataHScheduleData64, we're not using this call here but you could use
	// 64-bit versions first, and if they fail fall back to the older calls.
	OSType err;
  err = DataHScheduleData(m_dataHandler,	// DataHandler Component Instance
                          (Ptr)buffer,	// Specifies the location in memory that is to receive the data
                          position,	        	// Offset in the data reference from which you want to read
                          length,       // The number of bytes to read
                          0,            // refCon
                          NULL,         // pointer to a schedule record - NULL for Synchronous operation
                          NULL);        // pointer to a data-handler completion function - NULL for Syncronous operation
	if (err)
    return -2;

  // if we read less than what caller asked for, then return error.
  //if (size < size_t(length))
  //  return -1;

  return 0;
}

//--------------------------------------------------------------------------------
int MkvReaderQT::Length(long long* total, long long* available)
{
  if (total)
    *total = m_length;

  if (available)
    *available = m_length;

  return 0;
}



//--------------------------------------------------------------------------------
//
MkvBufferedReaderQT::MkvBufferedReaderQT() :
  m_buffer(), m_bufpos(0), m_chunksize(kDefaultChunkSize)
{
}

//--------------------------------------------------------------------------------
MkvBufferedReaderQT::~MkvBufferedReaderQT()
{
}

//--------------------------------------------------------------------------------
//
int MkvBufferedReaderQT::Read(long long requestedPos, long requestedLen, unsigned char* outbuf)
{
  dbg_printf("MkvBufferedReaderQT::Read() - requestedPos = %lld, requestedLen = %ld\n", requestedPos, requestedLen);
  dbg_printf("\tm_bufpos = %lld, m_buffer.size = %lu\n", m_bufpos, m_buffer.size());

  if (requestedPos != m_bufpos) {
    dbg_printf("\tNON-CONTIGUOUS READ, empty the buffer\n");
    // non-contiguous read
    // empty buffer
    size_t numToPop = m_buffer.size();
    for (long i=0; i < numToPop; i++) {
      m_buffer.pop();
    }
    // reset position even though empty queue
    m_bufpos = requestedPos;
  }

  dbg_printf("\tm_bufpos=%lld, requestedLen=%ld, m_buffer.size() = %lu\n", m_bufpos, requestedLen, m_buffer.size());

  // contiguous read (or empty buffer)
  // is the request larger than what we already have in buffer?
  if (requestedLen > m_buffer.size()) {
    dbg_printf("\tNOT ENOUGH IN BUFFER, read from file.\n");
    // not enough in buffer...
    // read from data handler (file)
    size_t growSize = (requestedLen < m_chunksize) ? m_chunksize : requestedLen; // grow buffer by chunksize or requestedLen, whichever is greater.
    unsigned char* tempBuf = (unsigned char*)malloc(growSize);
    int err = this->MkvReaderQT::Read(requestedPos, growSize, tempBuf);
    if (err != 0) {
      free(tempBuf);
      return err;
    }

    // append to existing buffer
    for (long i=0; i < growSize; i++) {
      m_buffer.push(tempBuf[i]);
    }

    free(tempBuf);
  }

  dbg_printf("\tm_buffer.front = %1x\n", m_buffer.front());

  // read from buffer
  if (requestedLen > 0) {
    for (long i=0; i < requestedLen; i++) {
      outbuf[i] = m_buffer.front();
      m_buffer.pop();
      m_bufpos++;
    }
  }

  dbg_printf("\toutbuf[0]=%1x\n", outbuf[0]);
  dbg_printf("MkvBufferedReaderQT::Read() return.\n\n");
  return 0;
}


