#pragma once

#include <QObject>
#include <QMutex>
#include <QImage>
#include <QByteArray>
#include <QSharedMemory>

#include <cstdint>

struct AVFrame;

class Settings;

// Low-latency decoded-frame tap for automation / AI consumers.
//
// Design goals (autonomous play with minimal latency):
// - Zero extra copies when disabled (publishFrame early-outs).
// - Decoder-thread publishing: called right after hw->sw transfer, before the
//   frame is handed to the GUI thread, so AI sees it as early as possible.
// - Same-machine fast path via shared memory (BGRA, no JPEG encode).
// - On-demand JPEG for HTTP polling (/api/frame.jpg) with per-quality cache.
// - Optional downscale to a small AI size (default 960x540) to cut memcpy,
//   conversion and encode cost.
//
// Thread-safe: publishFrame() runs on the decoder/frame thread, readers run on
// the automation HTTP thread / GUI thread. All state guarded by mutex.
// Disabled by default; enable via settings frame_publish_enabled (which the
// automation server also honors). When disabled, publishFrame() is a no-op.
class FramePublisher : public QObject
{
	Q_OBJECT
	public:
		static constexpr uint32_t kShmMagic = 0x43484941; // 'CHIA'
		static constexpr uint32_t kShmVersion = 1;

#pragma pack(push, 1)
		struct ShmHeader
		{
			uint32_t magic = kShmMagic;
			uint32_t version = kShmVersion;
			uint64_t seq = 0;
			int64_t timestamp_us = 0;
			double pts = 0.0;
			int32_t width = 0;
			int32_t height = 0;
			int32_t stride = 0;
			int32_t format = 0; // 0 = BGRA32
		};
#pragma pack(pop)

		explicit FramePublisher(Settings *settings, QObject *parent = nullptr);
		~FramePublisher() override;

		void setSettings(Settings *settings);

		// Refresh cached config from Settings. Must be called on the GUI thread
		// after settings change (QSettings is not thread-safe for concurrent
		// decoder-thread reads). publishFrame() only reads the cache.
		void refreshConfig();

		bool isEnabled() const;
		quint64 sequence() const;
		qint64 lastTimestampUs() const;
		void lastFrameInfo(int &w, int &h, quint64 &seq, qint64 &timestamp_us) const;

		// Publish a software-ready frame. Safe to call with nullptr. No-op when
		// frame publishing is disabled in settings.
		void publishFrame(const AVFrame *frame, double pts);

		// Latest frame as QImage (deep copy). Returns null image if none yet.
		QImage latestImage() const;
		// Latest frame JPEG-encoded at the requested quality (10-100). Cached per
		// (seq, quality) so repeated HTTP polls for the same frame are free.
		QByteArray latestJpeg(int quality = -1);
		// Latest raw BGRA buffer plus geometry. Returns false if none yet.
		bool latestRaw(QByteArray &out, int &w, int &h, quint64 &seq) const;

		QString shmKey() const { return shm_key; }

	signals:
		void framePublished(quint64 seq);

	private:
		bool enabledInSettings() const;
		void ensureShm(int w, int h);
		void writeShm(const QImage &img, quint64 seq, int64_t timestamp_us, double pts);

		Settings *settings = nullptr;
		mutable QMutex mutex;
		// Cached config (GUI thread writes, decoder thread reads under lock).
		bool cached_enabled = true;
		int cached_w = 960;
		int cached_h = 540;
		int cached_quality = 80;
		QImage latest_image;
		quint64 seq = 0;
		qint64 last_timestamp_us = 0;
		double last_pts = 0.0;
		QByteArray cached_jpeg;
		quint64 cached_jpeg_seq = 0;
		int cached_jpeg_quality = 0;
		QSharedMemory shm;
		// Per-process key (set in ctor with PID suffix); discoverable via
		// /api/frame.info so readers never guess.
		QString shm_key = QStringLiteral("chiaki-ng-frame");
		void *sws_ctx = nullptr;
		int sws_src_w = 0;
		int sws_src_h = 0;
		int sws_src_fmt = -1;
		int sws_dst_w = 0;
		int sws_dst_h = 0;
};
