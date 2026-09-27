#pragma once

#include <QObject>
#include <QMutex>
#include <QMap>
#include <QTcpServer>
#include <QUdpSocket>
#include <QHostAddress>

#include <cstdint>

class Settings;
class QmlBackend;
class QmlMainWindow;
class FramePublisher;
class QTcpSocket;

// Local automation / AI remote-control API.
//
// Goals:
// - Control the console with no noticeable delay (Req 2) and serve as the
//   interface between an autonomous AI and the console (Req 3, long-term:
//   FC 27 offline play).
// - Extremely low-latency frame delivery is handled by FramePublisher (shared
//   memory + on-demand JPEG); this server is the control + metadata plane.
//
// Transports:
// - HTTP JSON on 127.0.0.1:<automation_port> (default 19090, 0 = disabled).
//   Endpoints (all JSON unless noted):
//     GET  /api/status            session + automation + frame metadata
//     POST /api/controller        full controller state (lowest-latency HTTP path)
//     POST /api/button            {button, pressed} convenience
//     POST /api/clear             release automation input
//     POST /api/pin               {pin}
//     POST /api/home              go home
//     POST /api/mute              {muted?} toggle when omitted
//     POST /api/video_mode        {mode: normal|zoom|stretch}
//     POST /api/window_mode       {mode: windowed|maximized|fullscreen}
//     POST /api/connect           {index}
//     POST /api/disconnect        {sleep}
//     POST /api/idr               request IDR
//     GET  /api/frame.jpg?quality=&width=  JPEG snapshot (served from cache)
//     GET  /api/frame.raw         BGRA raw + X-Frame-* headers
//     GET  /api/frame.info        JSON frame metadata
// - UDP binary on 127.0.0.1:<automation_udp_port> (default 19091, 0 = disabled).
//   Always bound to loopback, even when the HTTP API listens on LAN: UDP has
//   no token auth by design (single datagram, no handshake), so it must never
//   be exposed to an untrusted network.
//   Fire-and-forget 20-byte packets for 60Hz+ AI loops (no JSON/TCP overhead):
//     u32 magic 0x43484941 ('CHIA'), u32 seq, u32 buttons, u8 l2, u8 r2,
//     i16 lx, i16 ly, i16 rx, i16 ry. Stale/duplicate seqs are dropped.
//
// Security/stability:
// - HTTP binds localhost only by default (env CHIAKI_AUTOMATION_LISTEN=0.0.0.0
//   opts into LAN binding). Optional token (settings automation_token, env
//   CHIAKI_AUTOMATION_TOKEN) enforced on HTTP via X-Chiaki-Token / ?token=.
//   The token is NOT checked on UDP; UDP stays on 127.0.0.1 unconditionally.
// - Disabled by default (settings automation_enabled=false). All handlers are
//   no-ops without a session and never throw into the stream path.
class AutomationServer : public QObject
{
	Q_OBJECT
	public:
		static constexpr uint32_t kUdpMagic = 0x43484941;
		static constexpr int kUdpPacketSize = 20;

		explicit AutomationServer(Settings *settings, QmlBackend *backend, QmlMainWindow *window, FramePublisher *publisher, QObject *parent = nullptr);
		~AutomationServer() override;

		void setSettings(Settings *settings);
		void setBackend(QmlBackend *backend);
		void setWindow(QmlMainWindow *window);
		void setPublisher(FramePublisher *publisher);

		bool isRunning() const { return tcp_running || udp_running; }
		int tcpPort() const { return tcp_port_actual; }
		int udpPort() const { return udp_port_actual; }

	public slots:
		void start();
		void stop();
		void restart();

	private slots:
		void onNewTcpConnection();
		void onTcpReadyRead();
		void onUdpReadyRead();

	private:
		struct HttpRequest
		{
			QString method;
			QString path;
			QMap<QString, QString> query;
			QMap<QString, QString> headers;
			QByteArray body;
			bool valid = false;
		};

		bool enabledInSettings() const;
		QString expectedToken() const;
		QHostAddress listenAddress() const;
		bool checkAuth(const HttpRequest &req) const;

		HttpRequest parseHttp(const QByteArray &raw, int &header_end) const;
		void handleTcpSocket(QTcpSocket *socket);
		void sendJson(QTcpSocket *socket, int code, const QByteArray &body, bool keep_alive);
		void sendRaw(QTcpSocket *socket, int code, const char *content_type, const QByteArray &body, bool keep_alive, const QMap<QByteArray, QByteArray> &extra_headers = {});
		void sendStatus(QTcpSocket *socket, int code, const QString &message, bool keep_alive);
		void route(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);

		// Handlers return true when they wrote a response.
		bool handleStatus(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleController(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleButton(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleClear(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handlePin(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleHome(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleMute(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleVideoMode(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleWindowMode(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleConnect(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleDisconnect(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleIdr(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleExclusive(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleAutomationLog(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleFrameJpg(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleFrameRaw(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);
		bool handleFrameInfo(const HttpRequest &req, QTcpSocket *socket, bool keep_alive);

		static uint32_t buttonNameToBit(const QString &name);
		static bool controllerStateFromJson(const QJsonObject &obj, struct ChiakiControllerState &out, QString &error);
		bool effectiveExclusive() const;
		void noteInput(const QString &source, const struct ChiakiControllerState &state, uint32_t udp_seq = 0);

		struct AutomationLogEntry
		{
			quint64 timestamp_us = 0;
			QString source;
			uint32_t buttons = 0;
			uint8_t l2 = 0;
			uint8_t r2 = 0;
			int16_t left_x = 0;
			int16_t left_y = 0;
			int16_t right_x = 0;
			int16_t right_y = 0;
			uint32_t udp_seq = 0;
		};
		void appendLog(const AutomationLogEntry &entry);

		Settings *settings = nullptr;
		QmlBackend *backend = nullptr;
		QmlMainWindow *window = nullptr;
		FramePublisher *publisher = nullptr;

		QTcpServer *tcp_server = nullptr;
		QUdpSocket *udp_socket = nullptr;
		bool tcp_running = false;
		bool udp_running = false;
		int tcp_port_actual = 0;
		int udp_port_actual = 0;
		uint32_t last_udp_seq = 0;
		bool last_udp_seq_valid = false;
		// UDP telemetry (GUI thread only): received vs accepted vs dropped.
		quint64 udp_received = 0;
		quint64 udp_accepted = 0;
		quint64 udp_dropped_stale = 0;
		quint64 udp_dropped_bad_magic = 0;
		quint64 udp_dropped_no_session = 0;
		quint64 udp_dropped_short = 0;
		// Last automation input, any transport (monotonic us, GUI thread only).
		quint64 last_input_us = 0;
		QString last_input_source;
		// Bounded recent-input ring for training-data capture (GUI thread only).
		QList<AutomationLogEntry> input_log;
		static constexpr int kInputLogMax = 1200;
		QMutex mutex;
};
