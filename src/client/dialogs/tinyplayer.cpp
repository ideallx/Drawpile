/*
   Drawpile - a collaborative drawing program.

   Copyright (C) 2014 Calle Laakkonen

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
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QGraphicsDropShadowEffect>

#include "dialogs/tinyplayer.h"

#include "ui_tinyplayer.h"

namespace dialogs {

TinyPlayer::TinyPlayer(QWidget *parent)
	: QWidget(parent, Qt::Tool | Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint), _dragpoint(-1, -1)
{
	hide();

	_ui = new Ui_TinyPlayer;

#if defined(Q_OS_UNIX) && !defined(Q_OS_OSX)
	// This custom shadow only looks nice on Linux window managers
	// TODO check under Wayland
	{
		QWidget *w = new QWidget(this);
		_ui->setupUi(w);
		auto *layout = new QHBoxLayout;
		setLayout(layout);
		layout->addWidget(w);

		auto *shadow = new QGraphicsDropShadowEffect;
		shadow->setBlurRadius(8);
		shadow->setColor(QColor(0, 0, 0, 200));
		shadow->setOffset(0);
		setAttribute(Qt::WA_TranslucentBackground);
		w->setGraphicsEffect(shadow);
	}

#else
	// On other platforms the player will get a small window frame
	_ui->setupUi(this);
#endif

	connect(_ui->prevMarker, SIGNAL(clicked()), this, SIGNAL(prevMarker()));
	connect(_ui->nextMarker, SIGNAL(clicked()), this, SIGNAL(nextMarker()));
	connect(_ui->play, SIGNAL(clicked(bool)), this, SIGNAL(playToggled(bool)));
	connect(_ui->step, SIGNAL(clicked()), this, SIGNAL(step()));
	connect(_ui->skip, SIGNAL(clicked()), this, SIGNAL(skip()));

	_ui->prevMarker->hide();
	_ui->nextMarker->hide();

	// Context menu
	_idxactions = new QActionGroup(this);
	_ctxmenu = new QMenu(this);
	_ctxmenu->addAction(tr("Normal Player"), this, SLOT(close()));
	_ctxmenu->addSeparator();
	_idxactions->addAction(_ctxmenu->addAction(tr("Previous Snapshot"), this, SIGNAL(prevSnapshot())));
	_idxactions->addAction(_ctxmenu->addAction(tr("Next Snapshot"), this, SIGNAL(nextSnapshot())));

	_idxactions->setEnabled(false);
}

TinyPlayer::~TinyPlayer()
{
	delete _ui;
}

void TinyPlayer::setMaxProgress(int max)
{
	_ui->progress->setMaximum(max);
}

void TinyPlayer::setProgress(int pos)
{
	_ui->progress->setValue(pos);
}

void TinyPlayer::setPlayback(bool play)
{
	_ui->play->setChecked(play);
}

void TinyPlayer::enableIndex()
{
	_ui->prevMarker->show();
	_ui->nextMarker->show();
	_idxactions->setEnabled(true);
}

void TinyPlayer::setMarkerMenu(QMenu *menu)
{
	_ctxmenu->addMenu(menu);
}

void TinyPlayer::mouseMoveEvent(QMouseEvent *event)
{
	if(_dragpoint.x()<0) {
		_dragpoint = event->pos();
		setCursor(Qt::SizeAllCursor);
	}
	move(event->globalPos() - _dragpoint);
}

void TinyPlayer::mouseReleaseEvent(QMouseEvent *)
{
	setCursor(QCursor());
	_dragpoint = QPoint(-1, -1);
}

void TinyPlayer::keyReleaseEvent(QKeyEvent *event)
{
	QWidget::keyReleaseEvent(event);
	if(event->key() == Qt::Key_Escape)
		close();
}

void TinyPlayer::contextMenuEvent(QContextMenuEvent *event)
{
	_ctxmenu->popup(event->globalPos());
}

void TinyPlayer::closeEvent(QCloseEvent *e)
{
	emit hidden();
	QWidget::closeEvent(e);
}

}

