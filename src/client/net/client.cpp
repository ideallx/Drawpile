/*
   Drawpile - a collaborative drawing program.

   Copyright (C) 2013-2015 Calle Laakkonen

   Drawpile is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Drawpile is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Drawpile.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <QDebug>
#include <QImage>

#include "net/client.h"
#include "net/loopbackserver.h"
#include "net/tcpserver.h"
#include "net/utils.h"
#include "net/login.h"
#include "net/userlist.h"
#include "net/layerlist.h"

#include "statetracker.h"

#include "core/point.h"

#include "../shared/net/annotation.h"
#include "../shared/net/image.h"
#include "../shared/net/layer.h"
#include "../shared/net/meta.h"
#include "../shared/net/flow.h"
#include "../shared/net/pen.h"
#include "../shared/net/snapshot.h"
#include "../shared/net/undo.h"
#include "../shared/net/recording.h"

using protocol::MessagePtr;

namespace net {

Client::Client(QObject *parent)
	: QObject(parent), _my_id(1)
{
	_loopback = new LoopbackServer(this);
	_server = _loopback;
	_isloopback = true;
	_isOp = false;
	_isSessionLocked = false;
	_isUserLocked = false;

	_userlist = new UserListModel(this);
	_layerlist = new LayerListModel(this);

	_layerlist->setMyId(_my_id);

	connect(_loopback, &LoopbackServer::messageReceived, this, &Client::handleMessage);
	connect(_layerlist, &LayerListModel::layerOrderChanged, this, &Client::sendLayerReorder);
}

Client::~Client()
{
}

void Client::connectToServer(LoginHandler *loginhandler)
{
	Q_ASSERT(_isloopback);

	TcpServer *server = new TcpServer(this);
	_server = server;
	_isloopback = false;
	_sessionId = loginhandler->sessionId(); // target host/join ID (if known already)

	connect(server, SIGNAL(loggingOut()), this, SIGNAL(serverDisconnecting()));
	connect(server, SIGNAL(serverDisconnected(QString, QString, bool)), this, SLOT(handleDisconnect(QString, QString, bool)));
	connect(server, SIGNAL(serverDisconnected(QString, QString, bool)), loginhandler, SLOT(serverDisconnected()));
	connect(server, SIGNAL(loggedIn(QString, int, bool)), this, SLOT(handleConnect(QString, int, bool)));
	connect(server, SIGNAL(messageReceived(protocol::MessagePtr)), this, SLOT(handleMessage(protocol::MessagePtr)));

	connect(server, SIGNAL(expectingBytes(int)), this, SIGNAL(expectingBytes(int)));
	connect(server, SIGNAL(bytesReceived(int)), this, SIGNAL(bytesReceived(int)));
	connect(server, SIGNAL(bytesSent(int)), this, SIGNAL(bytesSent(int)));
	connect(server, SIGNAL(lagMeasured(qint64)), this, SIGNAL(lagMeasured(qint64)));

	if(loginhandler->mode() == LoginHandler::HOST)
		loginhandler->setUserId(_my_id);

	emit serverConnected(loginhandler->url().host(), loginhandler->url().port());
	server->login(loginhandler);

	_lastToolCtx = drawingboard::ToolContext();
}

void Client::disconnectFromServer()
{
	_server->logout();
}

bool Client::isLoggedIn() const
{
	return _server->isLoggedIn();
}

QString Client::sessionId() const
{
	return _sessionId;
}

QUrl Client::sessionUrl(bool includeUser) const
{
	if(!isConnected())
		return QUrl();

	QUrl url = static_cast<const TcpServer*>(_server)->url();
	url.setScheme("drawpile");
	if(!includeUser)
		url.setUserInfo(QString());
	url.setPath("/" + sessionId());
	return url;
}

void Client::handleConnect(QString sessionId, int userid, bool join)
{
	_sessionId = sessionId;
	_my_id = userid;
	_layerlist->setMyId(userid);

	// Joining: we'll get the correct layer list from the server
	if(join)
		_layerlist->clear();

	emit serverLoggedin(join);
}

void Client::handleDisconnect(const QString &message,const QString &errorcode, bool localDisconnect)
{
	Q_ASSERT(_server != _loopback);

	emit serverDisconnected(message, errorcode, localDisconnect);
	_userlist->clearUsers();
	_layerlist->unlockAll();
	static_cast<TcpServer*>(_server)->deleteLater();
	_server = _loopback;
	_isloopback = true;
	_isOp = false;
	_isSessionLocked = false;
	_isUserLocked = false;
	emit opPrivilegeChange(false);
	emit sessionConfChange(false, false, false, false);
	emit lockBitsChanged();
}

void Client::init()
{
	_layerlist->clear();
}

bool Client::isLocalServer() const
{
	return _server->isLocal();
}

QString Client::myName() const
{
	return _userlist->getUserById(_my_id).name;
}

int Client::uploadQueueBytes() const
{
	return _server->uploadQueueBytes();
}

void Client::sendCommand(protocol::MessagePtr msg)
{
	Q_ASSERT(msg->isCommand());
	emit drawingCommandLocal(msg);
	_server->sendMessage(msg);
}

void Client::sendCanvasResize(int top, int right, int bottom, int left)
{
	sendCommand(MessagePtr(new protocol::CanvasResize(
		_my_id,
		top, right, bottom, left
	)));
}

void Client::sendNewLayer(int id, int source, const QColor &fill, bool insert, bool copy, const QString &title)
{
	Q_ASSERT(id>0 && id<=0xffff);
	uint8_t flags = 0;
	if(copy) flags |= protocol::LayerCreate::FLAG_COPY;
	if(insert) flags |= protocol::LayerCreate::FLAG_INSERT;

	sendCommand(MessagePtr(new protocol::LayerCreate(_my_id, id, source, fill.rgba(), flags, title)));
}

void Client::sendLayerAttribs(int id, float opacity, paintcore::BlendMode::Mode blend)
{
	Q_ASSERT(id>=0 && id<=0xffff);
	sendCommand(MessagePtr(new protocol::LayerAttributes(_my_id, id, opacity*255, int(blend))));
}

void Client::sendLayerTitle(int id, const QString &title)
{
	Q_ASSERT(id>=0 && id<=0xffff);
	sendCommand(MessagePtr(new protocol::LayerRetitle(_my_id, id, title)));
}

void Client::sendLayerVisibility(int id, bool hide)
{
	// This one is actually a local only change
	_layerlist->setLayerHidden(id, hide);
	emit layerVisibilityChange(id, hide);
}

void Client::sendDeleteLayer(int id, bool merge)
{
	Q_ASSERT(id>=0 && id<=0xffff);
	sendCommand(MessagePtr(new protocol::LayerDelete(_my_id, id, merge)));
}

void Client::sendLayerReorder(const QList<uint16_t> &ids)
{
	Q_ASSERT(ids.size()>0);
	sendCommand(MessagePtr(new protocol::LayerOrder(_my_id, ids)));
}

void Client::sendToolChange(const drawingboard::ToolContext &ctx)
{
	if(ctx != _lastToolCtx) {
		sendCommand(brushToToolChange(_my_id, ctx.layer_id, ctx.brush));
		_lastToolCtx = ctx;
		if(ctx.brush.blendingMode() != 0) // color is not used in erase mode
			emit sentColorChange(ctx.brush.color1());
	}
}

void Client::sendStroke(const paintcore::Point &point)
{
	protocol::PenPointVector v(1);
	v[0] = pointToProtocol(point);
	sendCommand(MessagePtr(new protocol::PenMove(_my_id, v)));
}

void Client::sendStroke(const paintcore::PointVector &points)
{
	sendCommand(MessagePtr(new protocol::PenMove(_my_id, pointsToProtocol(points))));
}

void Client::sendPenup()
{
	sendCommand(MessagePtr(new protocol::PenUp(_my_id)));
}

/**
 * This one is a bit tricky, since the whole image might not fit inside
 * a single message. In that case, multiple PUTIMAGE commands will be sent.
 * 
 * @param layer layer onto which the image should be drawn
 * @param x image x coordinate
 * @param y imagee y coordinate
 * @param image image data
 */
void Client::sendImage(int layer, int x, int y, const QImage &image, paintcore::BlendMode::Mode mode)
{
	QList<protocol::MessagePtr> msgs = putQImage(_my_id, layer, x, y, image, mode);
	for(MessagePtr msg : msgs)
		sendCommand(msg);

	if(isConnected())
		emit sendingBytes(_server->uploadQueueBytes());
}

void Client::sendFillRect(int layer, const QRect &rect, const QColor &color, paintcore::BlendMode::Mode blend)
{
	sendCommand(MessagePtr(new protocol::FillRect(
		_my_id, layer,
		int(blend),
		rect.x(), rect.y(),
		rect.width(), rect.height(),
		color.rgba()
	)));
}

void Client::sendUndopoint()
{
	sendCommand(MessagePtr(new protocol::UndoPoint(_my_id)));
}

void Client::sendUndo(int actions, int override)
{
	Q_ASSERT(actions != 0);
	Q_ASSERT(actions >= -128 && actions <= 127);
	sendCommand(MessagePtr(new protocol::Undo(_my_id, override, actions)));
}

void Client::sendRedo(int actions, int override)
{
	sendUndo(-actions, override);
}

void Client::sendAnnotationCreate(int id, const QRect &rect)
{
	Q_ASSERT(id>0 && id <=0xffff);
	sendCommand(MessagePtr(new protocol::AnnotationCreate(
		_my_id,
		id,
		rect.x(),
		rect.y(),
		rect.width(),
		rect.height()
	)));
}

void Client::sendAnnotationReshape(int id, const QRect &rect)
{
	Q_ASSERT(id>0 && id <=0xffff);
	sendCommand(MessagePtr(new protocol::AnnotationReshape(
		_my_id,
		id,
		rect.x(),
		rect.y(),
		rect.width(),
		rect.height()
	)));
}

void Client::sendAnnotationEdit(int id, const QColor &bg, const QString &text)
{
	Q_ASSERT(id>0 && id <=0xffff);
	sendCommand(MessagePtr(new protocol::AnnotationEdit(
		_my_id,
		id,
		bg.rgba(),
		text
	)));
}

void Client::sendAnnotationDelete(int id)
{
	Q_ASSERT(id>0 && id <=0xffff);
	sendCommand(MessagePtr(new protocol::AnnotationDelete(_my_id, id)));
}

/**
 * @brief Send the session initialization command stream
 * @param commands snapshot point commands
 */
void Client::sendSnapshot(const QList<protocol::MessagePtr> commands)
{
	// Send ACK to indicate the rest of the data is on its way
	_server->sendMessage(MessagePtr(new protocol::SnapshotMode(protocol::SnapshotMode::ACK)));

	// The actual snapshot data will be sent in parallel with normal session traffic
	_server->sendSnapshotMessages(commands);

	emit sendingBytes(_server->uploadQueueBytes());
}

void Client::sendChat(const QString &message, bool announce, bool action)
{
	_server->sendMessage(MessagePtr(new protocol::Chat(_my_id, message, announce, action)));
}

void Client::sendOpCommand(const QString &command)
{
	_server->sendMessage(protocol::Chat::opCommand(_my_id, command));
}

void Client::sendLaserPointer(const QPointF &point, int trail)
{
	Q_ASSERT(trail>=0);
	_server->sendMessage(MessagePtr(new protocol::MovePointer(_my_id, point.x() * 4, point.y() * 4, trail)));
}

void Client::sendMarker(const QString &text)
{
	// Keep markers private
	handleMessage(MessagePtr(new protocol::Marker(_my_id, text)));
}

/**
 * @brief Send a list of commands to initialize the session in local mode
 * @param commands
 */
void Client::sendLocalInit(const QList<protocol::MessagePtr> commands)
{
	Q_ASSERT(_isloopback);
	foreach(protocol::MessagePtr msg, commands)
		_loopback->sendMessage(msg);
}

void Client::sendLockUser(int userid, bool lock)
{
	Q_ASSERT(userid>0 && userid<256);
	QString cmd;
	if(lock)
		cmd = "lock #";
	else
		cmd = "unlock #";
	cmd += QString::number(userid);

	sendOpCommand(cmd);
}

void Client::sendOpUser(int userid, bool op)
{
	Q_ASSERT(userid>0 && userid<256);
	QString cmd;
	if(op)
		cmd = "op #";
	else
		cmd = "deop #";
	cmd += QString::number(userid);

	sendOpCommand(cmd);
}

void Client::sendKickUser(int userid)
{
	Q_ASSERT(userid>0 && userid<256);
	sendOpCommand(QString("kick #%1").arg(userid));
}

void Client::sendSetSessionTitle(const QString &title)
{
	_server->sendMessage(MessagePtr(new protocol::SessionTitle(_my_id, title)));
}

void Client::sendLockSession(bool lock)
{
	sendOpCommand(QStringLiteral("lockboard ") + (lock ? "on" : "off"));
}

void Client::sendLockLayerControls(bool lock)
{
	sendOpCommand(QStringLiteral("locklayerctrl ") + (lock ? "on" : "off"));
}

void Client::sendCloseSession(bool close)
{
	sendOpCommand(QStringLiteral("logins ") + (close ? "off" : "on"));
}

void Client::sendLayerAcl(int layerid, bool locked, QList<uint8_t> exclusive)
{
	if(_isloopback) {
		// Allow layer locking in loopback mode. Exclusive access doesn't make any sense in this mode.
		_server->sendMessage(MessagePtr(new protocol::LayerACL(_my_id, layerid, locked, QList<uint8_t>())));

	} else {
		_server->sendMessage(MessagePtr(new protocol::LayerACL(_my_id, layerid, locked, exclusive)));
	}
}

void Client::playbackCommand(protocol::MessagePtr msg)
{
	if(_isloopback)
		_server->sendMessage(msg);
	else
		qWarning() << "tried to play back command in network mode";
}

void Client::endPlayback()
{
	_userlist->clearUsers();
}

void Client::handleMessage(protocol::MessagePtr msg)
{
	// Emit message as-is for recording
	emit messageReceived(msg);

	// Emit command stream messages for drawing
	if(msg->isCommand()) {
		emit drawingCommandReceived(msg);
		return;
	}

	// Handle meta messages here
	switch(msg->type()) {
	using namespace protocol;
	case MSG_SNAPSHOT:
		handleSnapshotRequest(msg.cast<SnapshotMode>());
		break;
	case MSG_CHAT:
		handleChatMessage(msg.cast<Chat>());
		break;
	case MSG_USER_JOIN:
		handleUserJoin(msg.cast<UserJoin>());
		break;
	case MSG_USER_ATTR:
		handleUserAttr(msg.cast<UserAttr>());
		break;
	case MSG_USER_LEAVE:
		handleUserLeave(msg.cast<UserLeave>());
		break;
	case MSG_SESSION_TITLE:
		emit sessionTitleChange(msg.cast<SessionTitle>().title());
		break;
	case MSG_SESSION_CONFIG:
		handleSessionConfChange(msg.cast<SessionConf>());
		break;
	case MSG_LAYER_ACL:
		handleLayerAcl(msg.cast<LayerACL>());
		break;
	case MSG_INTERVAL:
		/* intervals are used only when playing back recordings */
		break;
	case MSG_MOVEPOINTER:
		handleMovePointer(msg.cast<MovePointer>());
		break;
	case MSG_MARKER:
		handleMarkerMessage(msg.cast<Marker>());
		break;
	case MSG_DISCONNECT:
		handleDisconnectMessage(msg.cast<Disconnect>());
		break;
	default:
		qWarning() << "received unhandled meta command" << msg->type();
	}
}

void Client::handleSnapshotRequest(const protocol::SnapshotMode &msg)
{
	// The server should ever only send a REQUEST mode snapshot messages
	if(msg.mode() != protocol::SnapshotMode::REQUEST && msg.mode() != protocol::SnapshotMode::REQUEST_NEW) {
		qWarning() << "received unhandled snapshot mode" << msg.mode() << "message.";
		return;
	}

	emit needSnapshot(msg.mode() == protocol::SnapshotMode::REQUEST_NEW);
}

void Client::handleChatMessage(const protocol::Chat &msg)
{
	QString username;
	if(msg.contextId()==0)
		username = tr("Server");
	else
		username = _userlist->getUsername(msg.contextId());

	emit chatMessageReceived(
		username,
		msg.message(),
		msg.isAnnouncement(),
		msg.isAction(),
		msg.contextId() == _my_id
	);
}

void Client::handleMarkerMessage(const protocol::Marker &msg)
{
	emit markerMessageReceived(
		_userlist->getUsername(msg.contextId()),
		msg.text()
	);
}

void Client::handleDisconnectMessage(const protocol::Disconnect &msg)
{
	qDebug() << "Received disconnect notification! Reason =" << msg.reason() << "and message =" << msg.message();
	const QString message = msg.message();

	if(msg.reason() == protocol::Disconnect::KICK) {
		emit youWereKicked(message);
		return;
	}

	QString chat;
	if(msg.reason() == protocol::Disconnect::ERROR)
		chat = tr("A server error occurred!");
	else if(msg.reason() == protocol::Disconnect::SHUTDOWN)
		chat = tr("The server is shutting down!");
	else
		chat = "Unknown error";

	if(!message.isEmpty())
		chat += QString(" (%1)").arg(message);

	emit chatMessageReceived(tr("Server"), chat, false, false, false);
}

void Client::handleUserJoin(const protocol::UserJoin &msg)
{
	_userlist->addUser(User(msg.contextId(), msg.name(), msg.contextId() == _my_id));
	emit userJoined(msg.contextId(), msg.name());
}

void Client::handleUserAttr(const protocol::UserAttr &msg)
{
	if(msg.contextId() == _my_id) {
		_isOp = msg.isOp();
		_isUserLocked = msg.isLocked();
		emit opPrivilegeChange(msg.isOp());
		emit lockBitsChanged();
	}
	_userlist->updateUser(msg);
}

void Client::handleUserLeave(const protocol::UserLeave &msg)
{
	QString name = _userlist->getUserById(msg.contextId()).name;
	_userlist->removeUser(msg.contextId());
	emit userLeft(name);
}

void Client::handleSessionConfChange(const protocol::SessionConf &msg)
{
	_isSessionLocked = msg.isLocked();
	emit sessionConfChange(msg.isLocked(), msg.isLayerControlsLocked(), msg.isClosed(), msg.isChatPreserved());
	emit lockBitsChanged();
}

void Client::handleLayerAcl(const protocol::LayerACL &msg)
{
	_layerlist->updateLayerAcl(msg.id(), msg.locked(), msg.exclusive());
	emit lockBitsChanged();
}

void Client::handleMovePointer(const protocol::MovePointer &msg)
{
	emit userPointerMoved(msg.contextId(), QPointF(msg.x() / 4.0, msg.y() / 4.0), msg.persistence());
}

}
