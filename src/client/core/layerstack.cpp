/*
   Drawpile - a collaborative drawing program.

   Copyright (C) 2008-2015 Calle Laakkonen

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
#include <QPainter>
#include <QMimeData>
#include <QtConcurrent>
#include <QDataStream>

#include "annotation.h"
#include "layer.h"
#include "layerstack.h"
#include "tile.h"
#include "rasterop.h"

namespace paintcore {

LayerStack::LayerStack(QObject *parent)
	: QObject(parent), _width(0), _height(0), _viewmode(NORMAL), _viewlayeridx(0),
	  _onionskinsBelow(4), _onionskinsAbove(4), _onionskinTint(true), _viewBackgroundLayer(true)
{
}

LayerStack::~LayerStack()
{
	foreach(Layer *l, _layers)
		delete l;
}

void LayerStack::resize(int top, int right, int bottom, int left)
{
	const QSize oldsize(_width, _height);

	int newtop = -top;
	int newleft = -left;
	int newright = _width + right;
	int newbottom = _height + bottom;
	if(newtop >= newbottom || newleft >= newright) {
		qWarning() << "Invalid resize: borders reversed";
		return;
	}
	_width = newright - newleft;
	_height = newbottom - newtop;

	_xtiles = Tile::roundTiles(_width);
	_ytiles = Tile::roundTiles(_height);
	_dirtytiles = QBitArray(_xtiles*_ytiles, true);

	foreach(Layer *l, _layers)
		l->resize(top, right, bottom, left);

	if(left || top) {
		// Update annotation positions
		QPoint offset(left, top);
		foreach(Annotation *a, _annotations) {
			a->setRect(a->rect().translated(offset));
			emit annotationChanged(a->id());
		}
	}

	emit resized(left, top, oldsize);
}

/**
 * @param id ID of the new layer
 * @param source source layer ID (used when copy or insert is true)
 * @param color background color (used when copy is false)
 * @param insert if true, the new layer is inserted above source (source 0 inserts at the bottom of the stack)
 * @param copy if true, the layer content is copied from the source
 * @param name layer title
 * @return newly created layer or null in case of error
 */
Layer *LayerStack::createLayer(int id, int source, const QColor &color, bool insert, bool copy, const QString &name)
{
	if(_width<=0 || _height<=0) {
		// We tolerate this, but in normal operation the canvas size should be
		// set before creating any layers.
		qWarning("Layer created before canvas size was set!");
	}

	// Find source layer if specified
	int sourceIdx=-1;
	if(source>0) {
		for(int i=0;i<_layers.size();++i) {
			if(_layers.at(i)->id() == source) {
				sourceIdx = i;
				break;
			}
		}

		if(sourceIdx<0) {
			qWarning("Source layer %d not found!", source);
			return nullptr;
		}
	}

	// Create or copy new layer
	Layer *nl;
	if(copy) {
		if(sourceIdx<0) {
			qWarning("No layer copy source specified!");
			return nullptr;
		}

		nl = new Layer(*_layers.at(sourceIdx));
		nl->setTitle(name);
		nl->setId(id);

	} else {
		nl = new Layer(this, id, name, color, QSize(_width, _height));
	}

	// Insert the new layer in the appropriate spot
	if(insert)
		_layers.insert(sourceIdx+1, nl);
	else
		_layers.append(nl);

	// Dirty regions must be marked after the layer is in the stack
	if(copy)
		nl->markOpaqueDirty();
	else if(color.alpha()>0)
		markDirty();

	return nl;
}

/**
 * @param id layer ID
 * @return true if layer was found and deleted
 */
bool LayerStack::deleteLayer(int id)
{
	for(int i=0;i<_layers.size();++i) {
		if(_layers.at(i)->id() == id) {
			_layers.at(i)->markOpaqueDirty();
			delete _layers.takeAt(i);
			return true;
		}
	}
	return false;
}

/**
 * @param neworder list of layer IDs in the new order
 * @pre neworder must be a permutation of the current layer order
 */
void LayerStack::reorderLayers(const QList<uint16_t> &neworder)
{
	Q_ASSERT(neworder.size() == _layers.size());
	QList<Layer*> newstack;
	newstack.reserve(_layers.size());
	foreach(int id, neworder) {
		Layer *l = 0;
		for(int i=0;i<_layers.size();++i) {
			if(_layers.at(i)->id() == id) {
				l=_layers.takeAt(i);
				break;
			}
		}
		Q_ASSERT(l);
		newstack.append(l);
	}
	_layers = newstack;
	markDirty();
}

/**
 * @param id id of the layer that will be merged
 */
void LayerStack::mergeLayerDown(int id) {
	const Layer *top;
	Layer *btm=0;
	for(int i=0;i<_layers.size();++i) {
		if(_layers[i]->id() == id) {
			top = _layers[i];
			if(i>0)
				btm = _layers[i-1];
			break;
		}
	}
	if(btm==0)
		qWarning() << "Tried to merge bottom-most layer";
	else
		btm->merge(top);
}

Layer *LayerStack::getLayerByIndex(int index)
{
	return _layers.at(index);
}

const Layer *LayerStack::getLayerByIndex(int index) const
{
	return _layers.at(index);
}

Layer *LayerStack::getLayer(int id)
{
	// Since the expected number of layers is always fairly low,
	// we can get away with a simple linear search. (Note that IDs
	// may appear in random order due to layers being moved around.)
	foreach(Layer *l, _layers)
		if(l->id() == id)
			return l;
	return 0;
}

const Layer *LayerStack::getLayer(int id) const
{
	for(const Layer *l : _layers)
		if(l->id() == id)
			return l;
	return nullptr;
}

Annotation *LayerStack::getAnnotation(int id)
{
	foreach(Annotation *a, _annotations)
		if(a->id() == id)
			return a;
	return 0;
}

Annotation *LayerStack::addAnnotation(int id, const QRect &initialrect)
{
	if(getAnnotation(id) != 0) {
		qWarning() << "Tried to add annotation" << id << "which already exists!";
		return 0;
	}

	Annotation *a = new Annotation(id);
	a->setRect(initialrect);
	_annotations.append(a);
	emit annotationChanged(id);

	return a;
}

void LayerStack::reshapeAnnotation(int id, const QRect &newrect)
{
	Annotation *a = getAnnotation(id);
	if(!a) {
		qWarning() << "Tried to reshape non-existent annotation" << id;
		return;
	}
	a->setRect(newrect);
	emit annotationChanged(id);
}

void LayerStack::changeAnnotation(int id, const QString &newtext, const QColor &bgcolor)
{
	Annotation *a = getAnnotation(id);
	if(!a) {
		qWarning() << "Tried to change non-existent annotation" << id;
		return;
	}
	a->setText(newtext);
	a->setBackgroundColor(bgcolor);
	emit annotationChanged(id);
}

void LayerStack::deleteAnnotation(int id)
{
	Annotation *a = getAnnotation(id);
	if(!a) {
		qWarning() << "Tried to delete non-existent annotation" << id;
		return;
	}
	_annotations.removeAll(a);
	delete a;
	emit annotationChanged(id);
}

/**
 * @param id layer id
 * @return layer index. Returns a negative index if layer is not found
 */
int LayerStack::indexOf(int id) const
{
	for(int i=0;i<_layers.size();++i)
		if(_layers.at(i)->id() == id)
			return i;
	return -1;
}

void LayerStack::setLayerHidden(int layerid, bool hide)
{
	Layer *l = getLayer(layerid);
	if(l) {
		l->setHidden(hide);
	} else {
		qWarning() << "Tried to set hidden flag of non-existent layer" << layerid;
	}
}

namespace {

struct UpdateTile {
	UpdateTile() : x(-1), y(-1) {}
	UpdateTile(int x_, int y_) : x(x_), y(y_) {}

	int x, y;
	quint32 data[Tile::LENGTH];
};

}

/**
 * The dirty flag for each painted tile will be cleared.
 *
 * @param rect area of the image to limit repainting to (rounded upwards to tile boundaries)
 * @param target device to paint onto
 */
void LayerStack::paintChangedTiles(const QRect& rect, QPaintDevice *target)
{
	if(_width<=0 || _height<=0)
		return;

	// Affected tile range
	const int tx0 = qBound(0, rect.left() / Tile::SIZE, _xtiles-1);
	const int tx1 = qBound(tx0, rect.right() / Tile::SIZE, _xtiles-1);
	const int ty0 = qBound(0, rect.top() / Tile::SIZE, _ytiles-1);
	const int ty1 = qBound(ty0, rect.bottom() / Tile::SIZE, _ytiles-1);

	// Gather list of tiles in need of updating
	QList<UpdateTile*> updates;

	for(int ty=ty0;ty<=ty1;++ty) {
		const int y = ty*_xtiles;
		for(int tx=tx0;tx<=tx1;++tx) {
			const int i = y+tx;
			if(_dirtytiles.testBit(i)) {
				updates.append(new UpdateTile(tx, ty));
				_dirtytiles.clearBit(i);
			}
		}
	}

	if(!updates.isEmpty()) {
		// Flatten tiles
		QtConcurrent::blockingMap(updates, [this](UpdateTile *t) {
			Tile::fillChecker(t->data, QColor(128,128,128), Qt::white);
			flattenTile(t->data, t->x, t->y);
		});

		// Paint flattened tiles
		QPainter painter(target);
		painter.setCompositionMode(QPainter::CompositionMode_Source);
		while(!updates.isEmpty()) {
			UpdateTile *ut = updates.takeLast();
			painter.drawImage(
				ut->x*Tile::SIZE,
				ut->y*Tile::SIZE,
				QImage(reinterpret_cast<const uchar*>(ut->data),
					Tile::SIZE, Tile::SIZE,
					QImage::Format_ARGB32
				)
			);
			delete ut;
		}
	}
}

Tile LayerStack::getFlatTile(int x, int y) const
{
	Tile t(Qt::transparent);
	flattenTile(t.data(), x, y);
	return t;
}

QColor LayerStack::colorAt(int x, int y) const
{
	if(_layers.isEmpty())
		return QColor();

	if(x<0 || y<0 || x>=_width || y>=_height)
		return QColor();

	// TODO some more efficient way of doing this
	Tile tile = getFlatTile(x/Tile::SIZE, y/Tile::SIZE);
	quint32 c = tile.data()[(y-Tile::roundDown(y)) * Tile::SIZE + (x-Tile::roundDown(x))];
	return QColor(c);
}

QImage LayerStack::toFlatImage(bool includeAnnotations) const
{
	Layer flat(nullptr, 0, QString(), Qt::transparent, QSize(_width, _height));

	foreach(const Layer *l, _layers)
		flat.merge(l, true);

	QImage image = flat.toImage();

	if(includeAnnotations) {
		QPainter painter(&image);
		foreach(Annotation *a, _annotations)
			a->paint(&painter);
	}

	return image;
}

QImage LayerStack::flatLayerImage(int layerIdx, bool useBgLayer, const QColor &background)
{
	Q_ASSERT(layerIdx>=0 && layerIdx < _layers.size());

	QScopedPointer<Layer> flat;

	if(useBgLayer)
		flat.reset(new Layer(*_layers.at(0)));
	else
		flat.reset(new Layer(nullptr, 0, QString(), background, QSize(_width, _height)));

	flat->merge(_layers.at(layerIdx), false);

	return flat->toImage();
}

// Flatten a single tile
void LayerStack::flattenTile(quint32 *data, int xindex, int yindex) const
{
	// Composite visible layers
	int layeridx = 0;
	foreach(const Layer *l, _layers) {
		if(isVisible(layeridx)) {
			const Tile &tile = l->tile(xindex, yindex);
			const quint32 tint = layerTint(layeridx);

			if(l->sublayers().count() || tint!=0) {
				// Sublayers (or tint) present, composite them first
				quint32 ldata[Tile::SIZE*Tile::SIZE];
				tile.copyTo(ldata);

				foreach(const Layer *sl, l->sublayers()) {
					if(sl->visible()) {
						const Tile &subtile = sl->tile(xindex, yindex);
						if(!subtile.isNull()) {
							compositePixels(sl->blendmode(), ldata, subtile.data(),
									Tile::SIZE*Tile::SIZE, sl->opacity());
						}
					}
				}

				if(tint)
					tintPixels(ldata, sizeof ldata / sizeof *ldata, tint);

				// Composite merged tile
				compositePixels(l->blendmode(), data, ldata,
						Tile::SIZE*Tile::SIZE, layerOpacity(layeridx));

			} else if(!tile.isNull()) {
				// No sublayers or tint, just this tile as it is
				compositePixels(l->blendmode(), data, tile.data(),
						Tile::SIZE*Tile::SIZE, layerOpacity(layeridx));
			}
		}

		++layeridx;
	}
}

void LayerStack::markDirty(const QRect &area)
{
	if(_layers.isEmpty() || _width<=0 || _height<=0)
		return;
	const int tx0 = qBound(0, area.left() / Tile::SIZE, _xtiles-1);
	const int tx1 = qBound(tx0, area.right() / Tile::SIZE, _xtiles-1) + 1;
	int ty0 = qBound(0, area.top() / Tile::SIZE, _ytiles-1);
	const int ty1 = qBound(ty0, area.bottom() / Tile::SIZE, _ytiles-1);
	
	for(;ty0<=ty1;++ty0) {
		_dirtytiles.fill(true, ty0*_xtiles + tx0, ty0*_xtiles + tx1);
	}
	_dirtyrect |= area;
}

void LayerStack::markDirty()
{
	if(_layers.isEmpty() || _width<=0 || _height<=0)
		return;
	_dirtytiles.fill(true);

	_dirtyrect = QRect(0, 0, _width, _height);
	notifyAreaChanged();
}

void LayerStack::markDirty(int x, int y)
{
	Q_ASSERT(x>=0 && x < _xtiles);
	Q_ASSERT(y>=0 && y < _ytiles);

	_dirtytiles.setBit(y*_xtiles + x);

	_dirtyrect |= QRect(x*Tile::SIZE, y*Tile::SIZE, Tile::SIZE, Tile::SIZE);
}

void LayerStack::markDirty(int index)
{
	Q_ASSERT(index>=0 && index < _dirtytiles.size());

	_dirtytiles.setBit(index);

	const int y = index / _xtiles;
	const int x = index % _xtiles;

	_dirtyrect |= QRect(x*Tile::SIZE, y*Tile::SIZE, Tile::SIZE, Tile::SIZE);
}

void LayerStack::notifyAreaChanged()
{
	if(!_dirtyrect.isEmpty()) {
		emit areaChanged(_dirtyrect);
		_dirtyrect = QRect();
	}
}

void LayerStack::setViewMode(ViewMode mode)
{
	if(mode != _viewmode) {
		_viewmode = mode;
		markDirty();
	}
}

void LayerStack::setViewLayer(int id)
{
	for(int i=0;i<_layers.size();++i) {
		if(_layers.at(i)->id() == id) {
			_viewlayeridx = i;
			if(_viewmode != NORMAL)
				markDirty();
			break;
		}
	}
}

void LayerStack::setOnionskinMode(int below, int above, bool tint)
{
	_onionskinsBelow = below;
	_onionskinsAbove = above;
	_onionskinTint = tint;

	if(_viewmode==ONIONSKIN)
		markDirty();
}

void LayerStack::setViewBackgroundLayer(bool usebg)
{
	_viewBackgroundLayer = usebg;
	if(_viewmode != NORMAL)
		markDirty();
}

int LayerStack::layerOpacity(int idx) const
{
	Q_ASSERT(idx>=0 && idx < _layers.size());
	int o = _layers.at(idx)->opacity();

	if(_viewBackgroundLayer && idx==0)
		return o;

	if(viewMode()==ONIONSKIN) {
		const int d = _viewlayeridx - idx;
		qreal rd;
		if(d<0 && d>=-_onionskinsAbove)
			rd = -d/qreal(_onionskinsAbove+1);
		else if(d>=0 && d <=_onionskinsBelow)
			rd = d/qreal(_onionskinsBelow+1);
		else
			return 0;

		return int(o * ((1-rd) * (1-rd)));
	}

	return o;
}

quint32 LayerStack::layerTint(int idx) const
{
	if(_onionskinTint && viewMode() == ONIONSKIN) {
		if(idx==0 && _viewBackgroundLayer)
			return 0;

		if(idx < _viewlayeridx)
			return 0x80ff3333;
		else if(idx > _viewlayeridx)
			return 0x803333ff;
	}

	return 0;
}

bool LayerStack::isVisible(int idx) const
{
	Q_ASSERT(idx>=0 && idx < _layers.size());
	if(!_layers.at(idx)->visible())
		return false;

	if(idx==0 && _viewBackgroundLayer)
		return true;
	switch(viewMode()) {
	case NORMAL: break;
	case SOLO: return idx == _viewlayeridx;
	case ONIONSKIN: return layerOpacity(idx) > 0;
	}

	return true;
}

Savepoint::~Savepoint()
{
	while(!layers.isEmpty())
		delete layers.takeLast();
	while(!annotations.isEmpty())
		delete annotations.takeLast();
}

Savepoint *LayerStack::makeSavepoint()
{
	Savepoint *sp = new Savepoint;
	foreach(Layer *l, _layers) {
		l->optimize();
		sp->layers.append(new Layer(*l));
	}

	foreach(Annotation *a, _annotations)
		sp->annotations.append(new Annotation(*a));

	sp->width = _width;
	sp->height = _height;

	return sp;
}

void LayerStack::restoreSavepoint(const Savepoint *savepoint)
{
	const QSize oldsize(_width, _height);
	if(_width != savepoint->width || _height != savepoint->height) {
		// Restore canvas size if it was different in the savepoint
		_width = savepoint->width;
		_height = savepoint->height;
		_xtiles = Tile::roundTiles(_width);
		_ytiles = Tile::roundTiles(_height);
		_dirtytiles = QBitArray(_xtiles*_ytiles, true);
		emit resized(0, 0, oldsize);
	} else {
		// Mark changed tiles as changed. Usually savepoints are quite close together
		// so most tiles will remain unchanged
		if(savepoint->layers.size() != _layers.size()) {
			// Layers added or deleted, just refresh everything
			// (force refresh even if layer stack is empty)
			_dirtytiles.fill(true);
			_dirtyrect = QRect(0, 0, _width, _height);

		} else {
			// Layer count has not changed, compare layer contents
			for(int l=0;l<savepoint->layers.size();++l) {
				const Layer *l0 = _layers.at(l);
				const Layer *l1 = savepoint->layers.at(l);
				if(l0->effectiveOpacity() != l1->effectiveOpacity()) {
					// Layer opacity has changed, refresh everything
					markDirty();
					break;
				}
				for(int i=0;i<_xtiles*_ytiles;++i) {
					// Note: An identity comparison works here, because the tiles
					// utilize copy-on-write semantics. Unchanged tiles will share
					// data pointers between savepoints.
					if(l0->tile(i) != l1->tile(i))
						markDirty(i);
				}
			}
		}
	}

	// Restore layers
	while(!_layers.isEmpty())
		delete _layers.takeLast();
	foreach(const Layer *l, savepoint->layers)
		_layers.append(new Layer(*l));

	// Restore annotations
	QSet<int> annotations;
	while(!_annotations.isEmpty()) {
		Annotation *a = _annotations.takeLast();
		annotations.insert(a->id());
		delete a;
	}
	foreach(const Annotation *a, savepoint->annotations) {
		annotations.insert(a->id());
		_annotations.append(new Annotation(*a));
	}

	foreach(int id, annotations)
		emit annotationChanged(id);


	notifyAreaChanged();
}

void Savepoint::toDatastream(QDataStream &out) const
{
	// Write size
	out << quint32(width) << quint32(height);

	// Write layers
	out << quint8(layers.size());
	foreach(const Layer *layer, layers) {
		layer->toDatastream(out);
	}

	// Write annotations
	out << quint16(annotations.size());
	foreach(const Annotation *annotation, annotations) {
		annotation->toDatastream(out);
	}
}

Savepoint *Savepoint::fromDatastream(QDataStream &in, LayerStack *owner)
{
	Savepoint *sp = new Savepoint;
	quint32 width, height;
	in >> width >> height;

	sp->width = width;
	sp->height = height;

	quint8 layers;
	in >> layers;
	while(layers--) {
		sp->layers.append(Layer::fromDatastream(owner, in));
	}

	quint16 annotations;
	in >> annotations;
	while(annotations--) {
		sp->annotations.append(Annotation::fromDatastream(in));
	}

	return sp;
}

}
