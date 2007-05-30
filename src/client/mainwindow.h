/*
   DrawPile - a collaborative drawing program.

   Copyright (C) 2006-2007 Calle Laakkonen

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include "tools.h"

class QActionGroup;
class QMessageBox;
class QUrl;
class QLabel;
class QSplitter;

namespace widgets {
	class NetStatus;
	class EditorView;
	class DualColorButton;
	class ToolSettings;
	class UserList;
	class ChatBox;
	class PaletteBox;
	class ColorBox;
}
namespace dialogs {
	class ColorDialog;
	class NewDialog;
	class HostDialog;
	class JoinDialog;
	class LoginDialog;
}
namespace drawingboard {
	class Board;
}

namespace network {
	class SessionState;
}

class Controller;

//! The application main window
class MainWindow : public QMainWindow {
	Q_OBJECT
	enum ErrorType {ERR_SAVE, ERR_OPEN, BAD_URL};
	public:
		MainWindow(const MainWindow *source=0);
		~MainWindow();

		//! Initialize the drawing board from an existing image
		void initBoard(const QImage& image);

		//! Initialize the drawing board from a file
		bool initBoard(const QString& filename);

		//! Initialize a blank drawing board
		void initBoard(const QSize& size, const QColor& color);

		//! Initialize a default drawing board
		void initDefaultBoard();

		//! Connect to a host and join a session if full URL is provided.
		void joinSession(const QUrl& url);

	public slots:
		//! Check if document is changed and show New dialog
		void showNew();
		//! Open the file
		void open(const QString& file);
		//! Show file open dialog
		void open();
		//! Save current document
		bool save();
		//! Save current document with a new name
		bool saveas();
		//! Show settings dialog
		void showSettings();
		//! Show host session dialog
		void host();
		//! Show join session dialog
		void join();
		//! Leave session (ask confirmation first)
		void leave();
		//! Zoom in
		void zoomin();
		//! Zoom out
		void zoomout();
		//! Reset to 1:1 zoom
		void zoomone();
		//! Toggle fullscreen mode
		void fullscreen(bool enable);
		//! Change current tool
		void selectTool(QAction *tool);
		//! Display about dialog
		void about();
		//! Display online help
		void help();
		//! Go to drawpile homepage
		void homepage();
	private slots:
		//! Show a color dialog and set foreground color
		void setForegroundColor();
		//! Show a color dialog and set background color
		void setBackgroundColor();
		//! Set session title
		void setSessionTitle(const QString& title);
		//! Create new document
		void newDocument();
		//! Open a recent file
		void openRecent(QAction *action);
		//! Mark unsaved changes
		void boardChanged();
		//! Cancel or start hosting
		void finishHost(int i);
		//! Cancel or join
		void finishJoin(int i);
		//! Leave session
		void finishLeave(int i);
		//! Logged in, host session
		void loggedinHost();
		//! Logged in, join session
		void loggedinJoin();
		//! Connection established
		void connected();
		//! Connection cut
		void disconnected();
		//! Joined a session
		void joined(network::SessionState *session);
		//! Disallow changes to the board
		void lock(const QString& reason);
		//! Allow changes to the board
		void unlock();
		//! New joins have been allowed/disallowed
		void allowJoins(bool allow);
		//! The user became session owner
		void becameOwner();
		//! Inform user about raster upload progress
		void rasterUp(int p);

	signals:
		//! This signal is emitted when the current tool is changed
		void toolChanged(tools::Type);

	protected:
		//! Handle closing of the main window
		void closeEvent(QCloseEvent *event);

	private:
		//! Check if the current board can be replaced
		bool canReplace() const;

		//! Add a new entry to recent files list
		void addRecentFile(const QString& file);

		//! Set the window title according to open file name
		void setTitle();

		//! Save settings and exit
		void exit();

		//! Display a standardised error message
		void showErrorMessage(ErrorType type);

		//! Display an error message
		void showErrorMessage(const QString& message, const QString& details=QString());

		//! Read settings from file/registry
		void readSettings();
		//! Clone settings from another MainWindow
		void cloneSettings(const MainWindow *source);
		//! Write settings to file/reqistry
		void writeSettings();

		//! Initialise QActions
		void initActions();
		//! Create menus
		void createMenus();
		//! Create toolbars
		void createToolbars();
		//! Create all dock windows
		void createDocks();
		//! Create tool settings dock
		void createToolSettings(QMenu *menu);
		//! Create user list dock
		void createUserList(QMenu *menu);
		//! Create palette dock
		void createPalette(QMenu *menu);
		//! Create color docks
		void createColorBoxes(QMenu *menu);
		//! Create dialogs
		void createDialogs();

		QSplitter *splitter_;
		widgets::ToolSettings *toolsettings_;
		widgets::UserList *userlist_;
		widgets::ChatBox *chatbox_;

		widgets::DualColorButton *fgbgcolor_;
		widgets::NetStatus *netstatus_;
		widgets::EditorView *view_;
		widgets::PaletteBox *palette_;
		widgets::ColorBox *rgb_, *hsv_;
		QLabel *lockstatus_;

		dialogs::ColorDialog *fgdialog_,*bgdialog_;
		dialogs::NewDialog *newdlg_;
		dialogs::HostDialog *hostdlg_;
		dialogs::JoinDialog *joindlg_;
		dialogs::LoginDialog *logindlg_;
		QMessageBox *msgbox_;
		QMessageBox *leavebox_;

		drawingboard::Board *board_;
		Controller *controller_;

		QString sessiontitle_;
		QString filename_;
		QString lastpath_;
		QString openimagenext_;

		QAction *new_;
		QAction *open_;
		QMenu *recent_;
		QAction *save_;
		QAction *saveas_;
		QAction *quit_;

		QAction *host_;
		QAction *join_;
		QAction *logout_;

		QActionGroup *adminTools_;
		QAction *lockboard_;
		QAction *disallowjoins_;

		QActionGroup *drawingtools_;
		QAction *brushtool_;
		QAction *erasertool_;
		QAction *pickertool_;
		QAction *linetool_;
		QAction *recttool_;
		QAction *zoomin_;
		QAction *zoomout_;
		QAction *zoomorig_;
		QAction *fullscreen_;
		QAction *settings_;

		QAction *toggleoutline_;
		QAction *togglecrosshair_;

		QAction *toolbartoggles_;
		QAction *docktoggles_;

		QAction *help_;
		QAction *homepage_;
		QAction *about_;
};

#endif

