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

#include <QImage>

#include "utils.h"
#include "../shared/net/image.h"
#include "../shared/net/pen.h"
#include "core/brush.h"

namespace {

// Split image into tile boundary aligned PutImages.
// These can be applied very efficiently when mode is MODE_REPLACE
void splitImageAtTileBoundaries(const int ctxid, const int layer, const int x, const int y, const QImage &image, paintcore::BlendMode::Mode mode, QList<protocol::MessagePtr> &list)
{
	static const int TILE = 64;

	const int x2=x+image.width();
	const int y2=y+image.height();

	int ty=y;
	int sy=0;
	while(ty<y2) {
		const int nextY = qMin(((ty + TILE) / TILE) * TILE, y2);
		int tx=x;
		int sx=0;
		while(tx<x2) {
			const int nextX = qMin(((tx + TILE) / TILE) * TILE, x2);

			const QImage i = image.copy(sx, sy, nextX-tx, nextY-ty);
			const QByteArray data = QByteArray::fromRawData(reinterpret_cast<const char*>(i.bits()), i.byteCount());

			QByteArray compressed = qCompress(data);
			Q_ASSERT(compressed.length() <= protocol::PutImage::MAX_LEN);

			list.append(protocol::MessagePtr(new protocol::PutImage(
				ctxid,
				layer,
				mode,
				tx,
				ty,
				i.width(),
				i.height(),
				compressed
			)));

			sx += nextX-tx;
			tx = nextX;
		}

		sy += nextY - ty;
		ty = nextY;
	}
}

bool isOpaque(const QImage &image)
{
	if(image.hasAlphaChannel()) {
		for(int y=0;y<image.height();++y)
			for(int x=0;x<image.width();++x)
				if(qAlpha(image.pixel(x,y)) < 255)
					return false;
	}

	return true;
}

// Recursively split image into small enough pieces.
// When mode is anything else than MODE_REPLACE, PutImage calls
// are expensive, so we want to split the image into as few pieces as possible.
void splitImage(int ctxid, int layer, int x, int y, const QImage &image, int mode, QList<protocol::MessagePtr> &list)
{
	Q_ASSERT(image.format() == QImage::Format_ARGB32);

	// Compress pixel data and see if it fits in a single message
	const QByteArray data = QByteArray::fromRawData(reinterpret_cast<const char*>(image.bits()), image.byteCount());
	QByteArray compressed = qCompress(data);

	if(compressed.length() > protocol::PutImage::MAX_LEN) {
		// Too big! Recursively divide the image and try sending those
		compressed = QByteArray(); // release data

		QImage i1, i2;
		int px, py;
		if(image.width() > image.height()) {
			px = image.width() / 2;
			py = 0;
			i1 = image.copy(0, 0, px, image.height());
			i2 = image.copy(px, 0, image.width()-px, image.height());
		} else {
			px = 0;
			py = image.height() / 2;
			i1 = image.copy(0, 0, image.width(), py);
			i2 = image.copy(0, py, image.width(), image.height()-py);
		}

		splitImage(ctxid, layer, x, y, i1, mode, list);
		splitImage(ctxid, layer, x+px, y+py, i2, mode, list);

	} else {
		// It fits! Send data!
		list.append(protocol::MessagePtr(new protocol::PutImage(
			ctxid,
			layer,
			mode,
			x,
			y,
			image.width(),
			image.height(),
			compressed
		)));
	}
}
}

namespace net {

/**
 * Multiple messages are generated if the image is too large to fit in just one.
 *
 * If target coordinates are less than zero, the image is automatically cropped.
 *
 * @param ctxid user context ID
 * @param layer target layer ID
 * @param x X coordinate
 * @param y Y coordinate
 * @param image the image to put
 * @param mode composition mode
 * @return
 */
QList<protocol::MessagePtr> putQImage(int ctxid, int layer, int x, int y, QImage image, paintcore::BlendMode::Mode mode)
{
	QList<protocol::MessagePtr> list;

	// Crop image if target coordinates are negative, since the protocol
	// does not support negative coordites.
	if(x<0 || y<0) {
		if(x < -image.width() || y < -image.height()) {
			// the entire image is outside the canvas
			return list;
		}

		int xoffset = x<0 ? -x : 0;
		int yoffset = y<0 ? -y : 0;
		image = image.copy(xoffset, yoffset, image.width()-xoffset, image.height()-yoffset);
		x += xoffset;
		y += yoffset;
	}

	// Optimization: if image is completely opaque, REPLACE mode is equivalent to NORMAL,
	// except potentially more efficient when split at tile boundaries
	if(mode == paintcore::BlendMode::MODE_NORMAL && isOpaque(image)) {
		mode = paintcore::BlendMode::MODE_REPLACE;
	}

	// Split image into pieces small enough to fit in a message
	image = image.convertToFormat(QImage::Format_ARGB32);
	if(mode == paintcore::BlendMode::MODE_REPLACE) {
		splitImageAtTileBoundaries(ctxid, layer, x, y, image, mode, list);
	} else {
		splitImage(ctxid, layer, x, y, image, mode, list);
	}

#ifndef NDEBUG
	double truesize = image.width() * image.height() * 4;
	double wiresize = 0;
	for(protocol::MessagePtr msg : list)
		wiresize += msg->length();
	qDebug("Sending a %.2f kb image in %d piece%s. Compressed size is %.2f kb. Compression ratio is %.1f%%",
		truesize/1024, list.size(), list.size()==1?"":"s", wiresize/1024, truesize/wiresize*100);
#endif
	return list;
}

protocol::MessagePtr brushToToolChange(int userid, int layer, const paintcore::Brush &brush)
{
	uint8_t mode = brush.subpixel() ? protocol::TOOL_MODE_SUBPIXEL : 0;
	mode |= brush.incremental() ? protocol::TOOL_MODE_INCREMENTAL : 0;

	return protocol::MessagePtr(new protocol::ToolChange(
		userid,
		layer,
		brush.blendingMode(),
		mode,
		brush.spacing(),
		brush.color1().rgba(),
		brush.color2().rgba(),
		brush.hardness1() * 255,
		brush.hardness2() * 255,
		brush.size1(),
		brush.size2(),
		brush.opacity1() * 255,
		brush.opacity2() * 255,
		brush.smudge1() * 255,
		brush.smudge2() * 255,
		brush.resmudge()
	));
}


/**
 * Convert a dpcore::Point to network format. The
 * reverse operation for this is in statetracker.cpp
 * @param p
 * @return
 */
protocol::PenPoint pointToProtocol(const paintcore::Point &point)
{
	int32_t x = point.x() * 4.0;
	int32_t y = point.y() * 4.0;
	uint16_t p = point.pressure() * 0xffff;

	return protocol::PenPoint(x, y, p);
}

/**
 * Convert a dpcore::Point to network format. The
 * reverse operation for this is in statetracker.cpp
 * @param p
 * @return
 */
protocol::PenPointVector pointsToProtocol(const paintcore::PointVector &points)
{
	protocol::PenPointVector ppvec;
	ppvec.reserve(points.size());
	foreach(const paintcore::Point &p, points)
		ppvec.append(pointToProtocol(p));

	return ppvec;
}


}
