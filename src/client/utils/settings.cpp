/*
   Drawpile - a collaborative drawing program.

   Copyright (C) 2015 Calle Laakkonen

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

#include "settings.h"

#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QApplication>

namespace utils {
namespace settings {

QString recordingFolder()
{
	QString path = QSettings().value("settings/recording/folder").toString();
	if(!path.isEmpty()) {
		QDir dir(path);
		if(dir.exists())
			return path;
	}

	return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

QStringList dataPaths()
{
	QStringList datapaths = QStandardPaths::standardLocations(QStandardPaths::DataLocation);

#ifdef Q_OS_MAC
	// On Mac, we look for data files in the application bundle
	datapaths << qApp->applicationDirPath() + "/../Resources";
#endif
	return datapaths;
}

}
}
