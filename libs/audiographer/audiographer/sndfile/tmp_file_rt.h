#ifndef AUDIOGRAPHER_TMP_FILE_RT_H
#define AUDIOGRAPHER_TMP_FILE_RT_H

#include <condition_variable>
#include <cstdio>
#include <string>

#include <glib.h>

#include "pbd/gstdio_compat.h"
#include "pbd/pthread_utils.h"
#include "pbd/ringbuffer.h"

#include "audiographer/flag_debuggable.h"
#include "audiographer/sink.h"
#include "sndfile_writer.h"
#include "sndfile_reader.h"

#include "tmp_file.h"

namespace AudioGrapher
{

	static const samplecnt_t rb_chunksize = 8192; // samples

/** A temporary file deleted after this class is destructed
 * with realtime safe background thread writer.
 */
template<typename T = DefaultSampleType>
class TmpFileRt
	: public TmpFile<T>
{
  public:

	/// \a filename_template must match the requirements for mkstemp, i.e. end in "XXXXXX"
	TmpFileRt (char * filename_template, int format, ChannelCount channels, samplecnt_t samplerate)
		: SndfileHandle (g_mkstemp(filename_template), true, SndfileBase::ReadWrite, format, channels, samplerate)
		, filename (filename_template)
  , _chunksize (rb_chunksize * channels)
  , _rb (std::max (_chunksize * 16, 5 * samplerate * channels))
	{
		init ();
	}

	using SndfileHandle::operator=;

	~TmpFileRt()
	{
		end_write ();
		/* explicitly close first, some OS (yes I'm looking at you windows)
		 * cannot delete files that are still open
		 */
		if (!filename.empty()) {
			SndfileBase::close();
			std::remove(filename.c_str());
		}
	}

	/// Writes data to file
	void process (ProcessContext<T> const & c)
	{
		SndfileWriter<T>::check_flags (*this, c);

		if (SndfileWriter<T>::throw_level (ThrowStrict) && c.channels() != SndfileHandle::channels()) {
			throw Exception (*this, boost::str (boost::format
				("Wrong number of channels given to process(), %1% instead of %2%")
				% c.channels() % SndfileHandle::channels()));
		}

		if (SndfileWriter<T>::throw_level (ThrowProcess) && _rb.write_space() < (size_t) c.samples()) {
			throw Exception (*this, boost::str (boost::format
				("Could not write data to ringbuffer/output file (%1%)")
				% SndfileHandle::strError()));
		}

		_rb.write (c.data(), c.samples());

		if (c.has_flag(ProcessContext<T>::EndOfInput)) {
			_capture = false;
			SndfileWriter<T>::FileWritten (filename);
		}

		if (_disk_thread_lock.try_lock()) {
			_data_ready.notify_one();
			_disk_thread_lock.unlock();
		}
	}

	using Sink<T>::process;

	void disk_thread ()
	{
		T *framebuf = (T*) malloc (_chunksize * sizeof (T));

		_disk_thread_lock.lock();

		while (_capture) {
			if ((samplecnt_t)_rb.read_space () >= _chunksize) {
				_rb.read (framebuf, _chunksize);
				samplecnt_t const written = SndfileBase::write (framebuf, _chunksize);
				assert (written == _chunksize);
				SndfileWriter<T>::samples_written += written;
			}
			if (!_capture) {
				break;
			}
			_data_ready.wait(_disk_thread_lock);
		}

		// flush ringbuffer
		while (_rb.read_space () > 0) {
			size_t remain = std::min ((samplecnt_t)_rb.read_space (), _chunksize);
			_rb.read (framebuf, remain);
			samplecnt_t const written = SndfileBase::write (framebuf, remain);
			SndfileWriter<T>::samples_written += written;
		}

		SndfileWriter<T>::writeSync();
		_disk_thread_lock.unlock();
		free (framebuf);
		TmpFile<T>::FileFlushed ();
	}

  protected:
	std::string filename;

	bool _capture;
	samplecnt_t _chunksize;
	PBD::RingBuffer<T> _rb;

	std::unique_lock<std::mutex> _disk_thread_lock;
	std::condition_variable      _data_ready;
	pthread_t _thread_id;

	static void * _disk_thread (void *arg)
	{
		TmpFileRt *d = static_cast<TmpFileRt *>(arg);
		pthread_set_name ("ExportDiskIO");
		d->disk_thread ();
		pthread_exit (0);
		return 0;
	}

	void end_write () {
		_disk_thread_lock.lock();
		_capture = false;
		_data_ready.notify_one();
		_disk_thread_lock.unlock();
		pthread_join (_thread_id, NULL);
	}

	void init()
	{
		SndfileWriter<T>::samples_written = 0;
		_capture = true;
		SndfileWriter<T>::add_supported_flag (ProcessContext<T>::EndOfInput);
		std::mutex inner_disk_thread_lock;
		_disk_thread_lock = std::unique_lock(inner_disk_thread_lock, std::defer_lock);

		if (pthread_create (&_thread_id, NULL, _disk_thread, this)) {
			_capture = false;
			if (SndfileWriter<T>::throw_level (ThrowStrict)) {
				throw Exception (*this, "Cannot create export disk writer");
			}
		}
	}

	private:
	TmpFileRt (TmpFileRt const & other) : SndfileHandle (other) {}
};

} // namespace

#endif // AUDIOGRAPHER_TMP_FILE_RT_H
