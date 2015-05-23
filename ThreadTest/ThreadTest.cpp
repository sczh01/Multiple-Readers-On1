// ThreadTest.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

#define DATAPEEKCOUNT  4
#define BINDATABLOCKSIZE 1024

const std::chrono::milliseconds interval(1123);
const std::chrono::milliseconds intervalShort(313);

struct WorkerData
{
	char binData[BINDATABLOCKSIZE];

	WorkerData()
	{
		//Initial fill
		GarbleData();
		PlaneData();
	}
	void GarbleData()
	{
		FILETIME ft;

		GetSystemTimeAsFileTime(&ft);
		binData[0] = (char)ft.dwLowDateTime;
		for (size_t ixb = 1; ixb < BINDATABLOCKSIZE; ixb++)
		{
			binData[ixb] = binData[ixb - 1] ^ (char)ft.dwHighDateTime;
			ft.dwHighDateTime = _rotl(ft.dwHighDateTime, 1);
		}
	}
	void PlaneData()
	{
		FILETIME ft;
		const size_t ixPlaneHere = BINDATABLOCKSIZE / 2;

		GetSystemTimeAsFileTime(&ft);
		for (size_t ixb2 = ixPlaneHere; ixb2 < ixPlaneHere + DATAPEEKCOUNT; ixb2++)
		{
			if (ixb2 > BINDATABLOCKSIZE) 
				break;
			binData[ixb2] = ft.dwLowDateTime & 1 ? 'a' : 'A';
			ft.dwLowDateTime >>= 1;
		}
	}
	bool getPlanedData(char *planedData, size_t cbPlanedData)
	{
		if (DATAPEEKCOUNT >= cbPlanedData)
			return false;	//Bad
		memset(planedData, 0, cbPlanedData);
		memcpy(planedData, binData + (BINDATABLOCKSIZE / 2), DATAPEEKCOUNT);
		return 0 == _stricmp("aaaa", planedData);
	}
};

template<class T>
class CDataProtector
{
public:
	T *m_wData;
	std::atomic<int> m_readerCount;
	std::atomic<int> m_writerCount;
	std::mutex m_writerMtx;
	std::atomic<int> m_unplanedCount;
	std::atomic_bool m_stop;

	CDataProtector(T *pData) : m_wData(pData)
	{
		m_readerCount = 0;
		m_unplanedCount = 0;
		m_stop.store(false);
	}

	void Stop()
	{
		m_stop.store(true);
	}
	bool IsStopping() 
	{
		return m_stop.load();
	}

private:
	CDataProtector();
};

class CScopeCount
{
public:
	CScopeCount(std::atomic<int> &countVar) : m_count(countVar)
	{
		m_count++;
	}
	virtual ~CScopeCount() { m_count--; }

private:
	CScopeCount();
	std::atomic<int> &m_count;
};

class CWorker
{
public:
	void workerFunc()
	{
		Log(__FUNCTION__ ": %p", this);
		doWork();
	}

	void LogText(const char *text)
	{
		puts(text);
	}
	void Log(const char *text, void *par)
	{
		char outText[80];

		StringCbPrintfA(outText, sizeof(outText), text, par);
		LogText(outText);
	}

	void start()
	{
		void (CWorker::*worker_)(void) = &CWorker::workerFunc;
		std::thread workerThread(worker_, this);
		m_workerThread.swap(workerThread);
	}

	void join()
	{
		m_workerThread.join();
	}

protected:
	virtual void doWork() = 0;
	void Nap()
	{
		std::this_thread::sleep_for(interval);
	}
	void ShortNap()
	{
		std::this_thread::sleep_for(intervalShort);
	}

private:
	std::thread m_workerThread;
};

class CReaderWorker : public CWorker
{
public:
	CReaderWorker(CDataProtector<WorkerData> *pData) : m_data(pData)
	{
	}
	virtual ~CReaderWorker()
	{
	}

protected:
	void waitOnWriters()
	{
		while ((0 < m_data->m_writerCount) && !m_data->IsStopping())
			Nap();
	}
	bool processData()
	{
		CScopeCount working(m_data->m_readerCount);
		char show[0x40], dataExcerpt[DATAPEEKCOUNT + 1];

		//Check if a writer is waiting
		if (m_data->m_writerMtx.try_lock())
			m_data->m_writerMtx.unlock();
		else
			return true;
		if (!m_data->m_wData->getPlanedData(dataExcerpt, _countof(dataExcerpt)))
		{
			LogText("DATA IS NOT PLANED!");
			m_data->m_unplanedCount++;
		}
		StringCbPrintfA(show, sizeof(show), "%s%p ", dataExcerpt, this);
		puts(show);
		return false;
	}
	virtual void doWork()
	{
		while (!m_data->IsStopping())
		{
			if (processData())
				waitOnWriters();	//Give space for writers
		}
	}
private:
	CDataProtector<WorkerData> * const m_data;
};

class CReaderWorkerPool
{
public:
	CReaderWorkerPool(CDataProtector<WorkerData> *pData, size_t count) : m_readerCount(count)
	{
		m_readers = new CReaderWorker *[m_readerCount];
		memset(m_readers, 0, count * sizeof(m_readers[0]));
		for (size_t ixReader = 0; ixReader < m_readerCount; ixReader++)
			m_readers[ixReader] = new CReaderWorker(pData);
	}
	virtual ~CReaderWorkerPool()
	{
		if (m_readers)
		{
			for (size_t ixReader = 0; ixReader < m_readerCount; ixReader++)
			{
				if (m_readers[ixReader])
					delete m_readers[ixReader];
			}
			delete[] m_readers;
		}
	}
	void StartAll()
	{
		if (m_readers) for (size_t ixReader = 0; ixReader < m_readerCount; ixReader++)
		{
			if (m_readers[ixReader])
				m_readers[ixReader]->start();
		}
	}
	void JoinAll()
	{
		if (m_readers) for (size_t ixReader = 0; ixReader < m_readerCount; ixReader++)
		{
			if (m_readers[ixReader])
				m_readers[ixReader]->join();
		}
	}

private:
	CReaderWorker **m_readers;
	const size_t m_readerCount;
};

class CWriterWorker : public CWorker
{
public:
	CWriterWorker(CDataProtector<WorkerData> *pData, int lazyLevel) : m_data(pData), m_lazyLevel(lazyLevel)
	{
	}
	virtual ~CWriterWorker()
	{
	}

protected:
	bool garbleAndPlane()
	{
		//First acquire the writer lock
		std::lock_guard<std::mutex> lockForWrite(m_data->m_writerMtx);
		CScopeCount writerCount(m_data->m_writerCount);

		//Wait for readers to exit
		while (m_data->m_readerCount > 0)
		{
			if (m_data->IsStopping())
				return false;
			ShortNap();
		}
		m_data->m_wData->GarbleData();
		ShortNap();
		m_data->m_wData->PlaneData();
		return true;
	}
	virtual void doWork()
	{
		LazyNap();
		while (!m_data->IsStopping())
		{
			if (garbleAndPlane())
				LogText("Garbled and planed.");
			LazyNap();
		}
	}
	void LazyNap()
	{
		for (int ixNap = 0; (ixNap < m_lazyLevel) && !m_data->IsStopping(); ixNap++)
			Nap();
	}
private:
	CDataProtector<WorkerData> * const m_data;
	const int m_lazyLevel;
};


int wmain(int argc, WCHAR* argv[])
{
	WorkerData wData;
	CDataProtector<WorkerData> dataProtector(&wData);
	CReaderWorkerPool readers(&dataProtector, 8);
	CWriterWorker writer1(&dataProtector, 3), writer2(&dataProtector, 5);
	size_t runTime = 15667;

	if (argc > 1)
	{
		WCHAR *dumch;
		runTime = wcstol(argv[1], &dumch, 10);
	}
	printf("Running for %lu seconds", runTime / 1000);
	readers.StartAll();
	writer1.start(); writer2.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(runTime));
	puts("Stopping workers...\r\n");
	dataProtector.Stop();
	readers.JoinAll();
	writer1.join(); writer2.join();
	printf("Unplaned count: %u\r\n", (int)dataProtector.m_unplanedCount);
	return 0;
}

