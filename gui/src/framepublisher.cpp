// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "framepublisher.h"
#include "settings.h"

#include <QtGlobal>
#include <QCoreApplication>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}
#if __has_include(<libswscale/swscale.h>)
extern "C" {
#include <libswscale/swscale.h>
}
#define CHIAKI_HAVE_SWSCALE 1
#else
#define CHIAKI_HAVE_SWSCALE 0
#endif
extern "C" {
#include <chiaki/time.h>
}

#include <QBuffer>
#include <QMutexLocker>

FramePublisher::FramePublisher(Settings *settings, QObject *parent)
	: QObject(parent)
	, settings(settings)
	, shm(shm_key)
{
	// Unique SHM key per process so two instances never publish into (or read
	// from) each other's segment. Readers discover the key via /api/frame.info.
	shm_key = QStringLiteral("chiaki-ng-frame-%1").arg(QCoreApplication::applicationPid());
	shm.setKey(shm_key);
	refreshConfig();
}

FramePublisher::~FramePublisher()
{
#if CHIAKI_HAVE_SWSCALE
	if(sws_ctx)
	{
		sws_freeContext(static_cast<SwsContext *>(sws_ctx));
		sws_ctx = nullptr;
	}
#endif
	if(shm.isAttached())
		shm.detach();
}

void FramePublisher::setSettings(Settings *s)
{
	{
		QMutexLocker locker(&mutex);
		settings = s;
		cached_jpeg_seq = 0;
		cached_jpeg.clear();
	}
	refreshConfig();
}

void FramePublisher::refreshConfig()
{
	// GUI thread only: QSettings access must not race with decoder thread.
	Settings *s = nullptr;
	{
		QMutexLocker locker(&mutex);
		s = settings;
	}
	bool enabled = true;
	int w = 960, h = 540, q = 80;
	if(s)
	{
		enabled = s->GetFramePublishEnabled();
		w = qBound(160, s->GetFramePublishWidth(), 3840);
		h = qBound(90, s->GetFramePublishHeight(), 2160);
		q = qBound(10, s->GetFramePublishQuality(), 100);
	}
	{
		QMutexLocker locker(&mutex);
		cached_enabled = enabled;
		cached_w = w;
		cached_h = h;
		cached_quality = q;
	}
}

bool FramePublisher::isEnabled() const
{
	QMutexLocker locker(&mutex);
	return cached_enabled;
}

bool FramePublisher::enabledInSettings() const
{
	QMutexLocker locker(&mutex);
	return cached_enabled;
}

quint64 FramePublisher::sequence() const
{
	QMutexLocker locker(&mutex);
	return seq;
}

qint64 FramePublisher::lastTimestampUs() const
{
	QMutexLocker locker(&mutex);
	return last_timestamp_us;
}

void FramePublisher::lastFrameInfo(int &w, int &h, quint64 &s, qint64 &ts) const
{
	QMutexLocker locker(&mutex);
	w = latest_image.width();
	h = latest_image.height();
	s = seq;
	ts = last_timestamp_us;
}

QImage FramePublisher::latestImage() const
{
	QMutexLocker locker(&mutex);
	return latest_image.copy();
}

QByteArray FramePublisher::latestJpeg(int quality)
{
	quint64 s = 0;
	QImage img;
	{
		QMutexLocker locker(&mutex);
		if(latest_image.isNull())
			return {};
		if(quality <= 0)
			quality = cached_quality;
		quality = qBound(10, quality, 100);
		s = seq;
		if(s == cached_jpeg_seq && quality == cached_jpeg_quality && !cached_jpeg.isEmpty())
			return cached_jpeg;
		img = latest_image.copy();
	}
	QByteArray out;
	QBuffer buf(&out);
	buf.open(QIODevice::WriteOnly);
	// Qt's JPEG writer is fast (~2ms for 960x540 q80) and avoids new deps.
	img.save(&buf, "JPG", quality);
	buf.close();
	{
		QMutexLocker locker(&mutex);
		// Only cache if no newer frame arrived meanwhile.
		if(s == seq)
		{
			cached_jpeg = out;
			cached_jpeg_seq = s;
			cached_jpeg_quality = quality;
		}
	}
	return out;
}

bool FramePublisher::latestRaw(QByteArray &out, int &w, int &h, quint64 &s) const
{
	QMutexLocker locker(&mutex);
	if(latest_image.isNull())
		return false;
	const QImage conv = latest_image.convertToFormat(QImage::Format_ARGB32);
	w = conv.width();
	h = conv.height();
	s = seq;
	out.resize((qsizetype)conv.sizeInBytes());
	memcpy(out.data(), conv.constBits(), (size_t)out.size());
	return true;
}

void FramePublisher::ensureShm(int w, int h)
{
	const qsizetype need = (qsizetype)sizeof(ShmHeader) + (qsizetype)w * h * 4;
	if(shm.isAttached() && shm.size() == need)
		return;
	if(shm.isAttached())
		shm.detach();
	// QSharedMemory::create() truncates to size; fixed geometry from settings
	// keeps this stable (default 960x540 => ~2MB).
	if(!shm.create(need))
	{
		// Another process (previous instance) may own it: attach instead.
		shm.attach();
	}
}

void FramePublisher::writeShm(const QImage &img, quint64 s, int64_t ts_us, double pts)
{
	if(img.isNull())
		return;
	const QImage bgra = img.format() == QImage::Format_ARGB32 ? img : img.convertToFormat(QImage::Format_ARGB32);
	const int w = bgra.width();
	const int h = bgra.height();
	ensureShm(w, h);
	if(!shm.isAttached())
		return;
	if(shm.size() < (qsizetype)sizeof(ShmHeader) + (qsizetype)w * h * 4)
		return;
	if(!shm.lock())
		return;
	auto *hdr = static_cast<ShmHeader *>(shm.data());
	hdr->magic = kShmMagic;
	hdr->version = kShmVersion;
	hdr->seq = s;
	hdr->timestamp_us = ts_us;
	hdr->pts = pts;
	hdr->width = w;
	hdr->height = h;
	hdr->stride = w * 4;
	hdr->format = 0;
	uint8_t *dst = static_cast<uint8_t *>(shm.data()) + sizeof(ShmHeader);
	// QImage scanlines are contiguous for ARGB32 without padding beyond width*4
	// in practice; copy row by row to be safe.
	for(int y = 0; y < h; y++)
		memcpy(dst + (size_t)y * w * 4, bgra.constScanLine(y), (size_t)w * 4);
	shm.unlock();
}

void FramePublisher::publishFrame(const AVFrame *frame, double pts)
{
	if(!frame)
		return;
	bool enabled = false;
	int dst_w = 960, dst_h = 540;
	{
		QMutexLocker locker(&mutex);
		enabled = cached_enabled;
		dst_w = cached_w;
		dst_h = cached_h;
	}
	if(!enabled)
		return;
#if !CHIAKI_HAVE_SWSCALE
	(void)pts;
	return;
#else
	const int src_w = frame->width;
	const int src_h = frame->height;
	const int src_fmt = frame->format;
	if(src_w <= 0 || src_h <= 0)
		return;
	// Exact scale to configured size (AI pipelines expect fixed size).
	QImage out(dst_w, dst_h, QImage::Format_ARGB32);
	if(out.isNull())
		return;

	SwsContext *ctx = static_cast<SwsContext *>(sws_ctx);
	if(!ctx || src_w != sws_src_w || src_h != sws_src_h || src_fmt != sws_src_fmt || dst_w != sws_dst_w || dst_h != sws_dst_h)
	{
		ctx = sws_getCachedContext(ctx, src_w, src_h, static_cast<AVPixelFormat>(src_fmt),
			dst_w, dst_h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		sws_ctx = ctx;
		sws_src_w = src_w;
		sws_src_h = src_h;
		sws_src_fmt = src_fmt;
		sws_dst_w = dst_w;
		sws_dst_h = dst_h;
		if(!ctx)
			return;
	}
	uint8_t *dst_planes[4] = { out.bits(), nullptr, nullptr, nullptr };
	int dst_strides[4] = { out.bytesPerLine(), 0, 0, 0 };
	// Single conversion copy: decode -> scale/convert -> QImage.
	sws_scale(ctx, frame->data, frame->linesize, 0, src_h, dst_planes, dst_strides);

	const int64_t now_us = (int64_t)chiaki_time_now_monotonic_us();
	quint64 new_seq = 0;
	{
		QMutexLocker locker(&mutex);
		seq++;
		new_seq = seq;
		last_timestamp_us = now_us;
		last_pts = pts;
		latest_image = out;
		cached_jpeg_seq = 0; // invalidate JPEG cache
		cached_jpeg.clear();
	}
	writeShm(out, new_seq, now_us, pts);
	emit framePublished(new_seq);
#endif
}
