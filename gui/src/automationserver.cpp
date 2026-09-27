// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "automationserver.h"
#include "settings.h"
#include "qmlbackend.h"
#include "qmlmainwindow.h"
#include "streamsession.h"
#include "framepublisher.h"

#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>
#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QPointer>
#include <QDebug>
#include <QtEndian>
#include <cstring>

#include <chiaki/controller.h>
#include <chiaki/time.h>

namespace {

QByteArray httpReason(int code)
{
	switch(code)
	{
	case 200: return "OK";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 500: return "Internal Server Error";
	default: return "OK";
	}
}

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

AutomationServer::AutomationServer(Settings *settings, QmlBackend *backend, QmlMainWindow *window, FramePublisher *publisher, QObject *parent)
	: QObject(parent)
	, settings(settings)
	, backend(backend)
	, window(window)
	, publisher(publisher)
{
	tcp_server = new QTcpServer(this);
	udp_socket = new QUdpSocket(this);
	connect(tcp_server, &QTcpServer::newConnection, this, &AutomationServer::onNewTcpConnection);
	connect(udp_socket, &QUdpSocket::readyRead, this, &AutomationServer::onUdpReadyRead);
}

AutomationServer::~AutomationServer()
{
	stop();
}

void AutomationServer::setSettings(Settings *s) { settings = s; }
void AutomationServer::setBackend(QmlBackend *b) { backend = b; }
void AutomationServer::setWindow(QmlMainWindow *w) { window = w; }
void AutomationServer::setPublisher(FramePublisher *p) { publisher = p; }

bool AutomationServer::enabledInSettings() const
{
	if(!settings)
		return false;
	// Env overrides allow headless/AI bring-up without touching profiles:
	// CHIAKI_AUTOMATION_ENABLED=1, CHIAKI_AUTOMATION_PORT=19090, etc.
	if(qEnvironmentVariableIsSet("CHIAKI_AUTOMATION_ENABLED"))
		return qEnvironmentVariableIntValue("CHIAKI_AUTOMATION_ENABLED") != 0;
	return settings->GetAutomationEnabled();
}

bool AutomationServer::effectiveExclusive() const
{
	if(qEnvironmentVariableIsSet("CHIAKI_AUTOMATION_EXCLUSIVE"))
		return qEnvironmentVariableIntValue("CHIAKI_AUTOMATION_EXCLUSIVE") != 0;
	return settings ? settings->GetAutomationExclusive() : false;
}

void AutomationServer::noteInput(const QString &source, const ChiakiControllerState &state, uint32_t udp_seq)
{
	last_input_us = chiaki_time_now_monotonic_us();
	last_input_source = source;
	AutomationLogEntry entry;
	entry.timestamp_us = last_input_us;
	entry.source = source;
	entry.buttons = state.buttons;
	entry.l2 = state.l2_state;
	entry.r2 = state.r2_state;
	entry.left_x = state.left_x;
	entry.left_y = state.left_y;
	entry.right_x = state.right_x;
	entry.right_y = state.right_y;
	entry.udp_seq = udp_seq;
	appendLog(entry);
}

void AutomationServer::appendLog(const AutomationLogEntry &entry)
{
	input_log.append(entry);
	while(input_log.size() > kInputLogMax)
		input_log.removeFirst();
}

QString AutomationServer::expectedToken() const
{
	if(qEnvironmentVariableIsSet("CHIAKI_AUTOMATION_TOKEN"))
		return qEnvironmentVariable("CHIAKI_AUTOMATION_TOKEN");
	return settings ? settings->GetAutomationToken() : QString();
}

QHostAddress AutomationServer::listenAddress() const
{
	// Localhost-only by default. Set CHIAKI_AUTOMATION_LISTEN=0.0.0.0 to expose
	// on LAN (understood risk for AI rigs).
	const QString listen = qEnvironmentVariable("CHIAKI_AUTOMATION_LISTEN", "127.0.0.1");
	if(listen == "0.0.0.0" || listen.toLower() == "any")
		return QHostAddress::Any;
	return QHostAddress(listen);
}

static int effectivePort(Settings *s, bool udp)
{
	if(!s)
		return 0;
	if(udp)
	{
		if(qEnvironmentVariableIsSet("CHIAKI_AUTOMATION_UDP_PORT"))
			return qEnvironmentVariableIntValue("CHIAKI_AUTOMATION_UDP_PORT");
		return s->GetAutomationUdpPort();
	}
	if(qEnvironmentVariableIsSet("CHIAKI_AUTOMATION_PORT"))
		return qEnvironmentVariableIntValue("CHIAKI_AUTOMATION_PORT");
	return s->GetAutomationPort();
}

void AutomationServer::start()
{
	if(!enabledInSettings())
		return;
	const QHostAddress tcp_addr = listenAddress();
	// UDP never follows the LAN listen address: it carries no token auth, so
	// it stays on loopback unconditionally (see header).
	const QHostAddress udp_addr = QHostAddress::LocalHost;
	const int tcp_port = effectivePort(settings, false);
	const int udp_port = effectivePort(settings, true);
	if(tcp_port > 0 && !tcp_running)
	{
		if(tcp_server->listen(tcp_addr, (quint16)tcp_port))
		{
			tcp_running = true;
			tcp_port_actual = tcp_server->serverPort();
		}
		else
			qWarning() << "Automation HTTP API failed to listen on" << tcp_addr.toString() << tcp_port << tcp_server->errorString();
	}
	if(udp_port > 0 && !udp_running)
	{
		if(udp_socket->bind(udp_addr, (quint16)udp_port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint))
		{
			udp_running = true;
			udp_port_actual = udp_socket->localPort();
		}
		else
			qWarning() << "Automation UDP input failed to bind on" << udp_addr.toString() << udp_port << udp_socket->errorString();
	}
}

void AutomationServer::stop()
{
	if(tcp_server->isListening())
		tcp_server->close();
	tcp_running = false;
	tcp_port_actual = 0;
	if(udp_socket->state() == QAbstractSocket::BoundState)
		udp_socket->close();
	udp_running = false;
	udp_port_actual = 0;
	last_udp_seq_valid = false;
	last_udp_seq = 0;
	udp_received = udp_accepted = udp_dropped_stale = udp_dropped_bad_magic = udp_dropped_no_session = udp_dropped_short = 0;
	last_input_us = 0;
	last_input_source.clear();
	input_log.clear();
}

void AutomationServer::restart()
{
	stop();
	start();
}

bool AutomationServer::checkAuth(const HttpRequest &req) const
{
	const QString expected = expectedToken();
	if(expected.isEmpty())
		return true;
	auto it = req.headers.find(QStringLiteral("x-chiaki-token"));
	if(it != req.headers.end() && *it == expected)
		return true;
	auto qit = req.query.find(QStringLiteral("token"));
	if(qit != req.query.end() && *qit == expected)
		return true;
	// Authorization: Bearer <token>
	auto ait = req.headers.find(QStringLiteral("authorization"));
	if(ait != req.headers.end() && ait->startsWith(QStringLiteral("Bearer ")))
	{
		if(ait->mid(7).trimmed() == expected)
			return true;
	}
	return false;
}

AutomationServer::HttpRequest AutomationServer::parseHttp(const QByteArray &raw, int &header_end) const
{
	HttpRequest req;
	header_end = raw.indexOf("\r\n\r\n");
	if(header_end < 0)
		return req;
	const QByteArray head = raw.left(header_end);
	const QList<QByteArray> lines = head.split('\n');
	if(lines.isEmpty())
		return req;
	const QByteArray request_line = lines[0].trimmed();
	const QList<QByteArray> parts = request_line.split(' ');
	if(parts.size() < 2)
		return req;
	req.method = QString::fromLatin1(parts[0]).toUpper();
	QUrl url(QString::fromLatin1(parts[1]));
	// Support absolute-form (http://host/path) and origin-form (/path).
	req.path = url.path().isEmpty() ? QStringLiteral("/") : url.path();
	QUrlQuery q(url);
	for(const auto &pair : q.queryItems(QUrl::FullyDecoded))
		req.query.insert(pair.first.toLower(), pair.second);
	// Headers (lowercased keys).
	for(int i = 1; i < lines.size(); i++)
	{
		const QByteArray line = lines[i].trimmed();
		if(line.isEmpty())
			continue;
		const int colon = line.indexOf(':');
		if(colon < 0)
			continue;
		const QString key = QString::fromLatin1(line.left(colon).trimmed()).toLower();
		const QString val = QString::fromLatin1(line.mid(colon + 1).trimmed());
		req.headers.insert(key, val);
	}
	req.body = raw.mid(header_end + 4);
	req.valid = true;
	return req;
}

void AutomationServer::onNewTcpConnection()
{
	while(tcp_server->hasPendingConnections())
	{
		QTcpSocket *socket = tcp_server->nextPendingConnection();
		socket->setProperty("automation_buf", QByteArray());
		connect(socket, &QTcpSocket::readyRead, this, &AutomationServer::onTcpReadyRead);
		connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
	}
}

void AutomationServer::onTcpReadyRead()
{
	QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
	if(!socket)
		return;
	handleTcpSocket(socket);
}

void AutomationServer::handleTcpSocket(QTcpSocket *socket)
{
	QByteArray buf = socket->property("automation_buf").toByteArray();
	buf += socket->readAll();
	// Need full headers first.
	int header_end = -1;
	HttpRequest req = parseHttp(buf, header_end);
	if(!req.valid)
	{
		if(buf.size() > 65536)
		{
			socket->disconnectFromHost();
			return;
		}
		socket->setProperty("automation_buf", buf);
		return;
	}
	int content_length = 0;
	auto cl = req.headers.find(QStringLiteral("content-length"));
	if(cl != req.headers.end())
		content_length = cl->toInt();
	const int total_needed = header_end + 4 + content_length;
	if(buf.size() < total_needed)
	{
		// Wait for the rest of the body (localhost POSTs usually arrive whole).
		if(buf.size() > 4 * 1024 * 1024)
		{
			socket->disconnectFromHost();
			return;
		}
		socket->setProperty("automation_buf", buf);
		return;
	}
	req.body = buf.mid(header_end + 4, content_length);
	const QByteArray leftover = buf.mid(total_needed);
	socket->setProperty("automation_buf", leftover);
	// Keep-alive only when explicitly requested; default close (simplest, and
	// per-request latency on localhost is negligible vs UDP fast path).
	bool keep_alive = false;
	auto conn = req.headers.find(QStringLiteral("connection"));
	if(conn != req.headers.end() && conn->compare(QStringLiteral("keep-alive"), Qt::CaseInsensitive) == 0)
		keep_alive = true;
	if(!checkAuth(req))
	{
		sendStatus(socket, 401, QStringLiteral("missing or invalid token"), keep_alive);
		if(!keep_alive)
			socket->disconnectFromHost();
		return;
	}
	route(req, socket, keep_alive);
	if(!keep_alive)
		socket->disconnectFromHost();
	else if(!leftover.isEmpty())
	{
		QPointer<QTcpSocket> guard(socket);
		QMetaObject::invokeMethod(this, [this, guard]() {
			if(!guard)
				return;
			handleTcpSocket(guard);
		}, Qt::QueuedConnection);
	}
}

void AutomationServer::sendJson(QTcpSocket *socket, int code, const QByteArray &body, bool keep_alive)
{
	QByteArray head = "HTTP/1.1 " + QByteArray::number(code) + " " + httpReason(code) + "\r\n";
	head += "Content-Type: application/json\r\n";
	head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
	head += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
	head += "Access-Control-Allow-Origin: *\r\n";
	head += "\r\n";
	socket->write(head + body);
}

void AutomationServer::sendRaw(QTcpSocket *socket, int code, const char *content_type, const QByteArray &body, bool keep_alive, const QMap<QByteArray, QByteArray> &extra_headers)
{
	QByteArray head = "HTTP/1.1 " + QByteArray::number(code) + " " + httpReason(code) + "\r\n";
	head += QByteArray("Content-Type: ") + content_type + "\r\n";
	head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
	for(auto it = extra_headers.begin(); it != extra_headers.end(); ++it)
		head += it.key() + ": " + it.value() + "\r\n";
	head += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
	head += "Access-Control-Allow-Origin: *\r\n";
	head += "\r\n";
	socket->write(head);
	if(!body.isEmpty())
		socket->write(body);
}

void AutomationServer::sendStatus(QTcpSocket *socket, int code, const QString &message, bool keep_alive)
{
	QJsonObject obj;
	obj[QStringLiteral("ok")] = (code >= 200 && code < 300);
	obj[QStringLiteral("message")] = message;
	sendJson(socket, code, QJsonDocument(obj).toJson(QJsonDocument::Compact), keep_alive);
}

void AutomationServer::route(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	const QString p = req.path.toLower();
	if(p == QStringLiteral("/api/status"))
		{ handleStatus(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/controller"))
		{ handleController(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/button"))
		{ handleButton(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/clear"))
		{ handleClear(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/pin"))
		{ handlePin(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/home"))
		{ handleHome(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/mute"))
		{ handleMute(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/video_mode"))
		{ handleVideoMode(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/window_mode"))
		{ handleWindowMode(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/connect"))
		{ handleConnect(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/disconnect"))
		{ handleDisconnect(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/idr"))
		{ handleIdr(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/exclusive"))
		{ handleExclusive(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/automation/log") || p == QStringLiteral("/api/automation_log"))
		{ handleAutomationLog(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/frame.jpg") || p == QStringLiteral("/api/frame.jpeg"))
		{ handleFrameJpg(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/frame.raw"))
		{ handleFrameRaw(req, socket, keep_alive); return; }
	if(p == QStringLiteral("/api/frame.info") || p == QStringLiteral("/api/frame_info"))
		{ handleFrameInfo(req, socket, keep_alive); return; }
	// Root: human-readable hint (not JSON to help curl users).
	if(p == QStringLiteral("/") || p == QStringLiteral("/api") || p == QStringLiteral("/api/"))
	{
		QJsonObject obj;
		obj[QStringLiteral("ok")] = true;
		obj[QStringLiteral("service")] = QStringLiteral("chiaki-ng automation api");
		obj[QStringLiteral("docs")] = QStringLiteral("see docs/setup/ai-remote-api.md");
		sendJson(socket, 200, QJsonDocument(obj).toJson(QJsonDocument::Compact), keep_alive);
		return;
	}
	sendStatus(socket, 404, QStringLiteral("unknown endpoint ") + req.path, keep_alive);
}

static StreamSession *activeSession(QmlBackend *backend)
{
	if(!backend)
		return nullptr;
	return backend->qmlSession();
}

bool AutomationServer::handleStatus(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	(void)req;
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	out[QStringLiteral("tcp_port")] = tcp_port_actual;
	out[QStringLiteral("udp_port")] = udp_port_actual;
	out[QStringLiteral("udp_localhost_only")] = true;
	QJsonObject udp;
	udp[QStringLiteral("received")] = QString::number(udp_received);
	udp[QStringLiteral("accepted")] = QString::number(udp_accepted);
	udp[QStringLiteral("dropped_stale")] = QString::number(udp_dropped_stale);
	udp[QStringLiteral("dropped_bad_magic")] = QString::number(udp_dropped_bad_magic);
	udp[QStringLiteral("dropped_no_session")] = QString::number(udp_dropped_no_session);
	udp[QStringLiteral("dropped_short")] = QString::number(udp_dropped_short);
	udp[QStringLiteral("last_seq")] = last_udp_seq_valid ? QString::number(last_udp_seq) : QStringLiteral("none");
	out[QStringLiteral("udp")] = udp;
	QJsonObject sess;
	StreamSession *s = activeSession(backend);
	if(s)
	{
		sess[QStringLiteral("present")] = true;
		sess[QStringLiteral("connected")] = s->IsConnected();
		sess[QStringLiteral("connecting")] = s->IsConnecting();
		sess[QStringLiteral("host")] = s->GetHost();
		sess[QStringLiteral("bitrate")] = s->GetMeasuredBitrate();
		sess[QStringLiteral("packet_loss")] = s->GetAveragePacketLoss();
		sess[QStringLiteral("frames_lost")] = s->GetFramesLost();
		sess[QStringLiteral("muted")] = s->GetMuted();
		sess[QStringLiteral("automation_active")] = s->HasAutomationState();
		sess[QStringLiteral("automation_exclusive")] = s->IsAutomationExclusive();
		const ChiakiControllerState cur = s->GetAutomationState();
		QJsonObject automation_state;
		automation_state[QStringLiteral("buttons")] = QString::number(cur.buttons);
		automation_state[QStringLiteral("l2_state")] = cur.l2_state;
		automation_state[QStringLiteral("r2_state")] = cur.r2_state;
		automation_state[QStringLiteral("left_x")] = cur.left_x;
		automation_state[QStringLiteral("left_y")] = cur.left_y;
		automation_state[QStringLiteral("right_x")] = cur.right_x;
		automation_state[QStringLiteral("right_y")] = cur.right_y;
		sess[QStringLiteral("automation_state")] = automation_state;
	}
	else
		sess[QStringLiteral("present")] = false;
	out[QStringLiteral("session")] = sess;
	if(!last_input_source.isEmpty())
	{
		QJsonObject last_input;
		last_input[QStringLiteral("source")] = last_input_source;
		last_input[QStringLiteral("timestamp_us")] = QString::number(last_input_us);
		const quint64 now_us = chiaki_time_now_monotonic_us();
		last_input[QStringLiteral("age_us")] = QString::number(now_us >= last_input_us ? now_us - last_input_us : 0);
		out[QStringLiteral("last_input")] = last_input;
	}
	QJsonObject frame;
	if(publisher)
	{
		int w = 0, h = 0;
		quint64 seq = 0;
		qint64 ts = 0;
		publisher->lastFrameInfo(w, h, seq, ts);
		frame[QStringLiteral("seq")] = QString::number(seq);
		frame[QStringLiteral("width")] = w;
		frame[QStringLiteral("height")] = h;
		frame[QStringLiteral("timestamp_us")] = QString::number(ts);
		frame[QStringLiteral("shm_key")] = publisher->shmKey();
	}
	out[QStringLiteral("frame")] = frame;
	if(settings)
	{
		out[QStringLiteral("window_mode")] = settings->GetStreamWindowMode() == StreamWindowMode::Fullscreen ? "fullscreen"
			: (settings->GetStreamWindowMode() == StreamWindowMode::Maximized ? "maximized" : "windowed");
	}
	if(window)
	{
		const int vm = static_cast<int>(window->videoMode());
		out[QStringLiteral("video_mode")] = (vm == 2 ? "zoom" : (vm == 1 ? "stretch" : "normal"));
	}
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

uint32_t AutomationServer::buttonNameToBit(const QString &name)
{
	const QString n = name.toLower().trimmed();
	if(n == QStringLiteral("cross") || n == QStringLiteral("x")) return CHIAKI_CONTROLLER_BUTTON_CROSS;
	if(n == QStringLiteral("moon") || n == QStringLiteral("circle") || n == QStringLiteral("b")) return CHIAKI_CONTROLLER_BUTTON_MOON;
	if(n == QStringLiteral("box") || n == QStringLiteral("square")) return CHIAKI_CONTROLLER_BUTTON_BOX;
	if(n == QStringLiteral("pyramid") || n == QStringLiteral("triangle") || n == QStringLiteral("y")) return CHIAKI_CONTROLLER_BUTTON_PYRAMID;
	if(n == QStringLiteral("dpad_left") || n == QStringLiteral("left")) return CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
	if(n == QStringLiteral("dpad_right") || n == QStringLiteral("right")) return CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
	if(n == QStringLiteral("dpad_up") || n == QStringLiteral("up")) return CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
	if(n == QStringLiteral("dpad_down") || n == QStringLiteral("down")) return CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
	if(n == QStringLiteral("l1") || n == QStringLiteral("leftshoulder")) return CHIAKI_CONTROLLER_BUTTON_L1;
	if(n == QStringLiteral("r1") || n == QStringLiteral("rightshoulder")) return CHIAKI_CONTROLLER_BUTTON_R1;
	if(n == QStringLiteral("l3") || n == QStringLiteral("leftstick")) return CHIAKI_CONTROLLER_BUTTON_L3;
	if(n == QStringLiteral("r3") || n == QStringLiteral("rightstick")) return CHIAKI_CONTROLLER_BUTTON_R3;
	if(n == QStringLiteral("options") || n == QStringLiteral("start")) return CHIAKI_CONTROLLER_BUTTON_OPTIONS;
	if(n == QStringLiteral("share") || n == QStringLiteral("create") || n == QStringLiteral("back")) return CHIAKI_CONTROLLER_BUTTON_SHARE;
	if(n == QStringLiteral("touchpad")) return CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
	if(n == QStringLiteral("ps") || n == QStringLiteral("guide")) return CHIAKI_CONTROLLER_BUTTON_PS;
	if(n == QStringLiteral("l2")) return CHIAKI_CONTROLLER_ANALOG_BUTTON_L2;
	if(n == QStringLiteral("r2")) return CHIAKI_CONTROLLER_ANALOG_BUTTON_R2;
	return 0;
}

static int16_t parseStick(const QJsonValue &v)
{
	if(v.isDouble())
	{
		const double d = v.toDouble();
		// Normalized float path (-1..1) for AI convenience.
		if(d >= -1.0 && d <= 1.0)
			return (int16_t)qBound(-32768.0, d * 32767.0, 32767.0);
		return (int16_t)qBound(-32768.0, d, 32767.0);
	}
	return (int16_t)clampInt(v.toInt(), -32768, 32767);
}

bool AutomationServer::controllerStateFromJson(const QJsonObject &obj, ChiakiControllerState &out, QString &error)
{
	chiaki_controller_state_set_idle(&out);
	// buttons: uint32 bitmask and/or array of names.
	if(obj.contains(QStringLiteral("buttons")))
	{
		const QJsonValue bv = obj.value(QStringLiteral("buttons"));
		if(bv.isArray())
		{
			uint32_t bits = 0;
			for(const QJsonValue &e : bv.toArray())
			{
				if(e.isDouble())
					bits |= (uint32_t)e.toInt();
				else
				{
					const uint32_t b = buttonNameToBit(e.toString());
					if(!b)
					{
						error = QStringLiteral("unknown button: ") + e.toString();
						return false;
					}
					bits |= b;
				}
			}
			out.buttons = bits;
		}
		else if(bv.isDouble() || bv.isString())
		{
			bool ok = false;
			uint32_t bits = 0;
			if(bv.isDouble())
			{
				bits = (uint32_t)bv.toInt();
				ok = true;
			}
			else
			{
				bits = bv.toString().toUInt(&ok);
			}
			if(!ok)
			{
				error = QStringLiteral("invalid buttons field");
				return false;
			}
			out.buttons = bits;
		}
		else
		{
			error = QStringLiteral("invalid buttons field");
			return false;
		}
	}
	// Named button helpers: {"cross": true, "r1": false, ...} merged in.
	static const char *names[] = {"cross","moon","circle","box","square","pyramid","triangle","dpad_left","dpad_right","dpad_up","dpad_down","l1","r1","l3","r3","options","share","touchpad","ps","l2","r2", nullptr};
	// Accept common aliases too.
	QMap<QString, QString> alias;
	alias[QStringLiteral("x")] = QStringLiteral("cross");
	alias[QStringLiteral("b")] = QStringLiteral("moon");
	alias[QStringLiteral("y")] = QStringLiteral("pyramid");
	alias[QStringLiteral("start")] = QStringLiteral("options");
	alias[QStringLiteral("back")] = QStringLiteral("share");
	alias[QStringLiteral("guide")] = QStringLiteral("ps");
	for(int i = 0; names[i]; i++)
	{
		const QString key = QString::fromLatin1(names[i]);
		if(obj.contains(key) && obj.value(key).isBool())
		{
			const uint32_t bit = buttonNameToBit(key);
			if(obj.value(key).toBool())
				out.buttons |= bit;
			else
				out.buttons &= ~bit;
		}
	}
	for(auto it = alias.begin(); it != alias.end(); ++it)
	{
		if(obj.contains(it.key()))
		{
			const QJsonValue v = obj.value(it.key());
			if(v.isBool())
			{
				const uint32_t bit = buttonNameToBit(it.value());
				if(v.toBool())
					out.buttons |= bit;
				else
					out.buttons &= ~bit;
			}
		}
	}
	auto u8field = [&](const char *k, uint8_t &dst) {
		const QString key = QString::fromLatin1(k);
		if(obj.contains(key))
			dst = (uint8_t)clampInt(obj.value(key).toInt(), 0, 255);
	};
	u8field("l2_state", out.l2_state);
	u8field("r2_state", out.r2_state);
	// Convenience 0..255 aliases.
	if(obj.contains(QStringLiteral("l2")))
		out.l2_state = (uint8_t)clampInt(obj.value(QStringLiteral("l2")).toInt(), 0, 255);
	if(obj.contains(QStringLiteral("r2")))
		out.r2_state = (uint8_t)clampInt(obj.value(QStringLiteral("r2")).toInt(), 0, 255);
	// L2/R2 analog buttons imply full press when state > ~30 unless caller set bits.
	if(out.l2_state > 30)
		out.buttons |= CHIAKI_CONTROLLER_ANALOG_BUTTON_L2;
	if(out.r2_state > 30)
		out.buttons |= CHIAKI_CONTROLLER_ANALOG_BUTTON_R2;
	if(obj.contains(QStringLiteral("left_x"))) out.left_x = parseStick(obj.value(QStringLiteral("left_x")));
	if(obj.contains(QStringLiteral("left_y"))) out.left_y = parseStick(obj.value(QStringLiteral("left_y")));
	if(obj.contains(QStringLiteral("right_x"))) out.right_x = parseStick(obj.value(QStringLiteral("right_x")));
	if(obj.contains(QStringLiteral("right_y"))) out.right_y = parseStick(obj.value(QStringLiteral("right_y")));
	auto ffield = [&](const char *k, float &dst) {
		const QString key = QString::fromLatin1(k);
		if(obj.contains(key))
			dst = (float)obj.value(key).toDouble();
	};
	ffield("gyro_x", out.gyro_x);
	ffield("gyro_y", out.gyro_y);
	ffield("gyro_z", out.gyro_z);
	ffield("accel_x", out.accel_x);
	ffield("accel_y", out.accel_y);
	ffield("accel_z", out.accel_z);
	ffield("orient_x", out.orient_x);
	ffield("orient_y", out.orient_y);
	ffield("orient_z", out.orient_z);
	ffield("orient_w", out.orient_w);
	return true;
}

bool AutomationServer::handleController(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	if(req.method != QStringLiteral("POST"))
	{
		sendStatus(socket, 405, QStringLiteral("use POST"), keep_alive);
		return true;
	}
	StreamSession *s = activeSession(backend);
	if(!s)
	{
		sendStatus(socket, 409, QStringLiteral("no active session"), keep_alive);
		return true;
	}
	QJsonParseError perr{};
	const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
	if(perr.error != QJsonParseError::NoError || !doc.isObject())
	{
		sendStatus(socket, 400, QStringLiteral("invalid JSON: ") + perr.errorString(), keep_alive);
		return true;
	}
	ChiakiControllerState state;
	QString error;
	if(!controllerStateFromJson(doc.object(), state, error))
	{
		sendStatus(socket, 400, error, keep_alive);
		return true;
	}
	// Lowest-latency HTTP path: single state swap + coalesced feedback send.
	s->SetAutomationExclusive(effectiveExclusive());
	s->SetAutomationState(state);
	noteInput(QStringLiteral("http"), state);
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleButton(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	if(req.method != QStringLiteral("POST"))
	{
		sendStatus(socket, 405, QStringLiteral("use POST"), keep_alive);
		return true;
	}
	StreamSession *s = activeSession(backend);
	if(!s)
	{
		sendStatus(socket, 409, QStringLiteral("no active session"), keep_alive);
		return true;
	}
	QJsonParseError perr{};
	const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
	if(perr.error != QJsonParseError::NoError || !doc.isObject())
	{
		sendStatus(socket, 400, QStringLiteral("invalid JSON"), keep_alive);
		return true;
	}
	const QJsonObject obj = doc.object();
	const QString name = obj.value(QStringLiteral("button")).toString();
	if(name.isEmpty())
	{
		sendStatus(socket, 400, QStringLiteral("missing button"), keep_alive);
		return true;
	}
	const uint32_t bit = buttonNameToBit(name);
	if(!bit)
	{
		sendStatus(socket, 400, QStringLiteral("unknown button"), keep_alive);
		return true;
	}
	ChiakiControllerState cur = s->GetAutomationState();
	if(!s->HasAutomationState())
		chiaki_controller_state_set_idle(&cur);
	const bool pressed = obj.value(QStringLiteral("pressed")).toBool(true);
	if(pressed)
		cur.buttons |= bit;
	else
		cur.buttons &= ~bit;
	// Keep analog trigger bits consistent with states.
	if(bit == CHIAKI_CONTROLLER_ANALOG_BUTTON_L2)
		cur.l2_state = pressed ? 255 : 0;
	if(bit == CHIAKI_CONTROLLER_ANALOG_BUTTON_R2)
		cur.r2_state = pressed ? 255 : 0;
	s->SetAutomationExclusive(effectiveExclusive());
	s->SetAutomationState(cur);
	noteInput(QStringLiteral("http-button"), cur);
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleClear(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	(void)req;
	StreamSession *s = activeSession(backend);
	if(s)
	{
		s->ClearAutomationState();
		ChiakiControllerState idle;
		chiaki_controller_state_set_idle(&idle);
		noteInput(QStringLiteral("clear"), idle);
	}
	// Idempotent: ok even without a session so AI reset loops stay simple.
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handlePin(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	StreamSession *s = activeSession(backend);
	if(!s)
	{
		sendStatus(socket, 409, QStringLiteral("no active session"), keep_alive);
		return true;
	}
	QString pin;
	if(req.method == QStringLiteral("POST"))
	{
		QJsonParseError perr{};
		const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
		if(doc.isObject())
			pin = doc.object().value(QStringLiteral("pin")).toString();
	}
	if(pin.isEmpty())
		pin = req.query.value(QStringLiteral("pin"));
	if(pin.isEmpty())
	{
		sendStatus(socket, 400, QStringLiteral("missing pin"), keep_alive);
		return true;
	}
	s->SetLoginPIN(pin);
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleHome(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	(void)req;
	StreamSession *s = activeSession(backend);
	if(!s)
	{
		sendStatus(socket, 409, QStringLiteral("no active session"), keep_alive);
		return true;
	}
	s->GoHome();
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleMute(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	StreamSession *s = activeSession(backend);
	if(!s)
	{
		sendStatus(socket, 409, QStringLiteral("no active session"), keep_alive);
		return true;
	}
	bool target = !s->GetMuted();
	if(req.method == QStringLiteral("POST") && !req.body.isEmpty())
	{
		QJsonParseError perr{};
		const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
		if(doc.isObject() && doc.object().contains(QStringLiteral("muted")))
			target = doc.object().value(QStringLiteral("muted")).toBool();
	}
	else if(req.query.contains(QStringLiteral("muted")))
		target = (req.query.value(QStringLiteral("muted")) == QStringLiteral("1") || req.query.value(QStringLiteral("muted")) == QStringLiteral("true"));
	s->SetMuted(target);
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	out[QStringLiteral("muted")] = s->GetMuted();
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleVideoMode(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	if(!window)
	{
		sendStatus(socket, 409, QStringLiteral("no window"), keep_alive);
		return true;
	}
	QString mode = req.query.value(QStringLiteral("mode"));
	if(req.method == QStringLiteral("POST") && !req.body.isEmpty())
	{
		QJsonParseError perr{};
		const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
		if(doc.isObject() && doc.object().contains(QStringLiteral("mode")))
			mode = doc.object().value(QStringLiteral("mode")).toString();
	}
	mode = mode.toLower().trimmed();
	if(mode == QStringLiteral("normal"))
		window->setVideoMode(QmlMainWindow::VideoMode::Normal);
	else if(mode == QStringLiteral("zoom"))
		window->setVideoMode(QmlMainWindow::VideoMode::Zoom);
	else if(mode == QStringLiteral("stretch"))
		window->setVideoMode(QmlMainWindow::VideoMode::Stretch);
	else
	{
		sendStatus(socket, 400, QStringLiteral("mode must be normal|zoom|stretch"), keep_alive);
		return true;
	}
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleWindowMode(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	if(!window || !settings)
	{
		sendStatus(socket, 409, QStringLiteral("no window"), keep_alive);
		return true;
	}
	QString mode = req.query.value(QStringLiteral("mode"));
	if(req.method == QStringLiteral("POST") && !req.body.isEmpty())
	{
		QJsonParseError perr{};
		const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
		if(doc.isObject() && doc.object().contains(QStringLiteral("mode")))
			mode = doc.object().value(QStringLiteral("mode")).toString();
	}
	mode = mode.toLower().trimmed();
	StreamWindowMode target;
	if(mode == QStringLiteral("windowed"))
		target = StreamWindowMode::Windowed;
	else if(mode == QStringLiteral("maximized"))
		target = StreamWindowMode::Maximized;
	else if(mode == QStringLiteral("fullscreen"))
		target = StreamWindowMode::Fullscreen;
	else
	{
		sendStatus(socket, 400, QStringLiteral("mode must be windowed|maximized|fullscreen"), keep_alive);
		return true;
	}
	if(settings)
		settings->SetStreamWindowMode(target);
	// Route through the backend so resize/max-size constraints update together
	// with the chrome (window->applyStreamWindowMode alone would skip sizing).
	if(backend)
		backend->applyStreamWindowModeAndSizing(target);
	else if(window)
		window->applyStreamWindowMode(target);
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleConnect(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	if(!backend)
	{
		sendStatus(socket, 500, QStringLiteral("no backend"), keep_alive);
		return true;
	}
	if(activeSession(backend))
	{
		sendStatus(socket, 409, QStringLiteral("session already active"), keep_alive);
		return true;
	}
	int index = -1;
	if(req.method == QStringLiteral("POST") && !req.body.isEmpty())
	{
		QJsonParseError perr{};
		const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
		if(doc.isObject() && doc.object().contains(QStringLiteral("index")))
			index = doc.object().value(QStringLiteral("index")).toInt(-1);
	}
	if(index < 0 && req.query.contains(QStringLiteral("index")))
		index = req.query.value(QStringLiteral("index")).toInt();
	if(index < 0)
	{
		sendStatus(socket, 400, QStringLiteral("missing index"), keep_alive);
		return true;
	}
	backend->connectToHost(index);
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleDisconnect(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	if(!backend || !activeSession(backend))
	{
		sendStatus(socket, 409, QStringLiteral("no active session"), keep_alive);
		return true;
	}
	bool sleep = false;
	if(req.method == QStringLiteral("POST") && !req.body.isEmpty())
	{
		QJsonParseError perr{};
		const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
		if(doc.isObject() && doc.object().contains(QStringLiteral("sleep")))
			sleep = doc.object().value(QStringLiteral("sleep")).toBool();
	}
	else if(req.query.contains(QStringLiteral("sleep")))
		sleep = (req.query.value(QStringLiteral("sleep")) == QStringLiteral("1") || req.query.value(QStringLiteral("sleep")) == QStringLiteral("true"));
	backend->stopSession(sleep);
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleIdr(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	(void)req;
	StreamSession *s = activeSession(backend);
	if(!s)
	{
		sendStatus(socket, 409, QStringLiteral("no active session"), keep_alive);
		return true;
	}
	const bool ok = s->RequestIDR();
	QJsonObject out;
	out[QStringLiteral("ok")] = ok;
	sendJson(socket, ok ? 200 : 500, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleExclusive(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	bool target = effectiveExclusive();
	bool have_target = false;
	if(req.method == QStringLiteral("POST") && !req.body.isEmpty())
	{
		QJsonParseError perr{};
		const QJsonDocument doc = QJsonDocument::fromJson(req.body, &perr);
		if(doc.isObject() && doc.object().contains(QStringLiteral("exclusive")))
		{
			target = doc.object().value(QStringLiteral("exclusive")).toBool();
			have_target = true;
		}
	}
	if(!have_target && req.query.contains(QStringLiteral("exclusive")))
	{
		const QString v = req.query.value(QStringLiteral("exclusive")).toLower();
		target = (v == QStringLiteral("1") || v == QStringLiteral("true"));
		have_target = true;
	}
	if(req.method == QStringLiteral("POST") && have_target)
	{
		if(settings)
			settings->SetAutomationExclusive(target);
		StreamSession *s = activeSession(backend);
		if(s)
			s->SetAutomationExclusive(target);
	}
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	StreamSession *s = activeSession(backend);
	out[QStringLiteral("exclusive")] = s ? s->IsAutomationExclusive() : (settings ? settings->GetAutomationExclusive() : false);
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleAutomationLog(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	int limit = 200;
	if(req.query.contains(QStringLiteral("limit")))
		limit = qBound(1, req.query.value(QStringLiteral("limit")).toInt(), kInputLogMax);
	QJsonArray entries;
	const int start = qMax(0, input_log.size() - limit);
	for(int i = start; i < input_log.size(); i++)
	{
		const AutomationLogEntry &e = input_log.at(i);
		QJsonObject obj;
		obj[QStringLiteral("t_us")] = QString::number(e.timestamp_us);
		obj[QStringLiteral("source")] = e.source;
		obj[QStringLiteral("buttons")] = QString::number(e.buttons);
		obj[QStringLiteral("l2")] = e.l2;
		obj[QStringLiteral("r2")] = e.r2;
		obj[QStringLiteral("lx")] = e.left_x;
		obj[QStringLiteral("ly")] = e.left_y;
		obj[QStringLiteral("rx")] = e.right_x;
		obj[QStringLiteral("ry")] = e.right_y;
		if(e.udp_seq)
			obj[QStringLiteral("udp_seq")] = QString::number(e.udp_seq);
		entries.append(obj);
	}
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	out[QStringLiteral("entries")] = entries;
	out[QStringLiteral("count")] = entries.size();
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

bool AutomationServer::handleFrameJpg(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	if(!publisher)
	{
		sendStatus(socket, 409, QStringLiteral("no frame publisher"), keep_alive);
		return true;
	}
	int quality = settings ? settings->GetFramePublishQuality() : 80;
	if(req.query.contains(QStringLiteral("quality")))
		quality = qBound(10, req.query.value(QStringLiteral("quality")).toInt(), 100);
	const QByteArray jpeg = publisher->latestJpeg(quality);
	if(jpeg.isEmpty())
	{
		sendStatus(socket, 404, QStringLiteral("no frame yet"), keep_alive);
		return true;
	}
	int w = 0, h = 0;
	quint64 seq = 0;
	qint64 ts = 0;
	publisher->lastFrameInfo(w, h, seq, ts);
	QMap<QByteArray, QByteArray> extra;
	extra.insert("X-Frame-Seq", QByteArray::number(seq));
	extra.insert("X-Frame-Width", QByteArray::number(w));
	extra.insert("X-Frame-Height", QByteArray::number(h));
	extra.insert("X-Frame-Timestamp-Us", QByteArray::number(ts));
	sendRaw(socket, 200, "image/jpeg", jpeg, keep_alive, extra);
	return true;
}

bool AutomationServer::handleFrameRaw(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	(void)req;
	if(!publisher)
	{
		sendStatus(socket, 409, QStringLiteral("no frame publisher"), keep_alive);
		return true;
	}
	QByteArray raw;
	int w = 0, h = 0;
	quint64 seq = 0;
	if(!publisher->latestRaw(raw, w, h, seq))
	{
		sendStatus(socket, 404, QStringLiteral("no frame yet"), keep_alive);
		return true;
	}
	qint64 ts = publisher->lastTimestampUs();
	QMap<QByteArray, QByteArray> extra;
	extra.insert("X-Frame-Seq", QByteArray::number(seq));
	extra.insert("X-Frame-Width", QByteArray::number(w));
	extra.insert("X-Frame-Height", QByteArray::number(h));
	extra.insert("X-Frame-Timestamp-Us", QByteArray::number(ts));
	extra.insert("X-Frame-Format", "BGRA32");
	sendRaw(socket, 200, "application/octet-stream", raw, keep_alive, extra);
	return true;
}

bool AutomationServer::handleFrameInfo(const HttpRequest &req, QTcpSocket *socket, bool keep_alive)
{
	(void)req;
	QJsonObject out;
	out[QStringLiteral("ok")] = true;
	if(publisher)
	{
		int w = 0, h = 0;
		quint64 seq = 0;
		qint64 ts = 0;
		publisher->lastFrameInfo(w, h, seq, ts);
		out[QStringLiteral("seq")] = QString::number(seq);
		out[QStringLiteral("width")] = w;
		out[QStringLiteral("height")] = h;
		out[QStringLiteral("timestamp_us")] = QString::number(ts);
		out[QStringLiteral("shm_key")] = publisher->shmKey();
		out[QStringLiteral("shm_format")] = QStringLiteral("BGRA32");
	}
	sendJson(socket, 200, QJsonDocument(out).toJson(QJsonDocument::Compact), keep_alive);
	return true;
}

void AutomationServer::onUdpReadyRead()
{
	while(udp_socket->hasPendingDatagrams())
	{
		QByteArray datagram;
		datagram.resize((int)udp_socket->pendingDatagramSize());
		QHostAddress sender;
		quint16 sender_port = 0;
		udp_socket->readDatagram(datagram.data(), datagram.size(), &sender, &sender_port);
		udp_received++;
		StreamSession *s = activeSession(backend);
		if(!s)
		{
			udp_dropped_no_session++;
			continue;
		}
		if(datagram.size() < kUdpPacketSize)
		{
			udp_dropped_short++;
			continue;
		}
		// Layout (little-endian): magic u32, seq u32, buttons u32, l2 u8, r2 u8,
		// lx i16, ly i16, rx i16, ry i16.
		uint32_t magic = 0, seq = 0, buttons = 0;
		uint8_t l2 = 0, r2 = 0;
		int16_t lx = 0, ly = 0, rx = 0, ry = 0;
		memcpy(&magic, datagram.constData(), 4);
		memcpy(&seq, datagram.constData() + 4, 4);
		memcpy(&buttons, datagram.constData() + 8, 4);
		l2 = static_cast<uint8_t>(datagram[12]);
		r2 = static_cast<uint8_t>(datagram[13]);
		memcpy(&lx, datagram.constData() + 14, 2);
		memcpy(&ly, datagram.constData() + 16, 2);
		memcpy(&rx, datagram.constData() + 18, 2);
#if Q_BYTE_ORDER == Q_BIG_ENDIAN
		magic = qFromLittleEndian(magic);
		seq = qFromLittleEndian(seq);
		buttons = qFromLittleEndian(buttons);
		lx = qFromLittleEndian(lx);
		ly = qFromLittleEndian(ly);
		rx = qFromLittleEndian(rx);
		ry = qFromLittleEndian(ry);
#endif
		if(magic != kUdpMagic)
		{
			udp_dropped_bad_magic++;
			continue;
		}
		// Drop stale/duplicate packets (seq 0 = unsequenced, always accept).
		if(seq != 0 && last_udp_seq_valid)
		{
			if(seq == last_udp_seq)
			{
				udp_dropped_stale++;
				continue;
			}
			// Accept wrap-around: only drop when seq is behind within 1M window.
			if(seq < last_udp_seq && (last_udp_seq - seq) < 1000000u)
			{
				udp_dropped_stale++;
				continue;
			}
		}
		last_udp_seq = seq;
		last_udp_seq_valid = true;
		udp_accepted++;
		ChiakiControllerState state;
		chiaki_controller_state_set_idle(&state);
		state.buttons = buttons;
		state.l2_state = l2;
		state.r2_state = r2;
		state.left_x = lx;
		state.left_y = ly;
		state.right_x = rx;
		state.right_y = ry;
		s->SetAutomationExclusive(effectiveExclusive());
		s->SetAutomationState(state);
		noteInput(QStringLiteral("udp"), state, seq);
	}
}
