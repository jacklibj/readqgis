/***************************************************************************
                         qgscomposerlegend.cpp  -  description
                         ---------------------
    begin                : June 2008
    copyright            : (C) 2008 by Marco Hugentobler
    email                : marco dot hugentobler at karto dot baug dot ethz dot ch
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include <limits>

#include "qgscomposerlegend.h"
#include "qgscomposerlegenditem.h"
#include "qgscomposermap.h"
#include "qgslogger.h"
#include "qgsmaplayer.h"
#include "qgsmaplayerregistry.h"
#include "qgsmaprenderer.h"
#include "qgsrenderer.h" //for brush scaling
#include "qgssymbol.h"
#include "qgssymbolv2.h"
#include <QDomDocument>
#include <QDomElement>
#include <QPainter>

QgsComposerLegend::QgsComposerLegend( QgsComposition* composition )
    : QgsComposerItem( composition )
    , mTitle( tr( "Legend" ) )
    , mFontColor( QColor( 0, 0, 0 ) )
    , mBoxSpace( 2 )
    , mColumnSpace( 2 )
    , mGroupSpace( 2 )
    , mLayerSpace( 2 )
    , mSymbolSpace( 2 )
    , mIconLabelSpace( 2 )
    , mColumnCount( 1 )
    , mComposerMap( 0 )
    , mSplitLayer( false )
    , mEqualColumnWidth( false )
{
  //QStringList idList = layerIdList();
  //mLegendModel.setLayerSet( idList );

  mTitleFont.setPointSizeF( 16.0 );
  mGroupFont.setPointSizeF( 14.0 );
  mLayerFont.setPointSizeF( 12.0 );
  mItemFont.setPointSizeF( 12.0 );

  mSymbolWidth = 7;
  mSymbolHeight = 4;
  mWrapChar = "";
  mlineSpacing = 1.5;
  adjustBoxSize();

  connect( &mLegendModel, SIGNAL( layersChanged() ), this, SLOT( synchronizeWithModel() ) );
}

QgsComposerLegend::QgsComposerLegend(): QgsComposerItem( 0 ), mComposerMap( 0 )
{

}

QgsComposerLegend::~QgsComposerLegend()
{

}

void QgsComposerLegend::paint( QPainter* painter, const QStyleOptionGraphicsItem* itemStyle, QWidget* pWidget )
{
  Q_UNUSED( itemStyle );
  Q_UNUSED( pWidget );
  paintAndDetermineSize( painter );
}

QSizeF QgsComposerLegend::paintAndDetermineSize( QPainter* painter )
{
  QSizeF size( 0, 0 );
  QStandardItem* rootItem = mLegendModel.invisibleRootItem();
  if ( !rootItem ) return size;

  if ( painter )
  {
    painter->save();
    drawBackground( painter );
    painter->setPen( QPen( QColor( 0, 0, 0 ) ) );
  }

  QList<Atom> atomList = createAtomList( rootItem, mSplitLayer );

  setColumns( atomList );

  double maxColumnWidth = 0;
  if ( mEqualColumnWidth )
  {
    foreach ( Atom atom, atomList )
    {
      maxColumnWidth = qMax( atom.size.width(), maxColumnWidth );
    }
  }

  QSizeF titleSize = drawTitle();
  // Using mGroupSpace as space between legend title and first atom in column
  double columnTop = mBoxSpace + titleSize.height() + mGroupSpace;

  QPointF point( mBoxSpace, columnTop );
  bool firstInColumn = true;
  double columnMaxHeight = 0;
  double columnWidth = 0;
  int column = 0;
  foreach ( Atom atom, atomList )
  {
    if ( atom.column > column )
    {
      // Switch to next column
      if ( mEqualColumnWidth )
      {
        point.rx() += mColumnSpace + maxColumnWidth;
      }
      else
      {
        point.rx() += mColumnSpace + columnWidth;
      }
      point.ry() = columnTop;
      columnWidth = 0;
      column++;
      firstInColumn = true;
    }
    // Add space if necessary, unfortunately it depends on first nucleon
    if ( !firstInColumn )
    {
      point.ry() += spaceAboveAtom( atom );
    }

    drawAtom( atom, painter, point );
    columnWidth = qMax( atom.size.width(), columnWidth );

    point.ry() += atom.size.height();
    columnMaxHeight = qMax( point.y() - columnTop, columnMaxHeight );

    firstInColumn = false;
  }
  point.rx() += columnWidth + mBoxSpace;

  size.rheight() = columnTop + columnMaxHeight + mBoxSpace;
  size.rwidth() = point.x();

  // Now we know total width and can draw the title centered
  if ( !mTitle.isEmpty() )
  {
    // For multicolumn center if we stay in totalWidth, otherwise allign to left
    // and expand total width. With single column keep alligned to left be cause
    // it looks better alligned with items bellow instead of centered
    Qt::AlignmentFlag halignment;
    if ( mColumnCount > 1 && titleSize.width() + 2 * mBoxSpace < size.width() )
    {
      halignment = Qt::AlignHCenter;
      point.rx() = mBoxSpace + size.rwidth() / 2;
    }
    else
    {
      halignment = Qt::AlignLeft;
      point.rx() = mBoxSpace;
      size.rwidth() = qMax( titleSize.width() + 2 * mBoxSpace, size.width() );
    }
    point.ry() = mBoxSpace;
    drawTitle( painter, point, halignment );
  }

  //adjust box if width or height is to small
  if ( painter && size.height() > rect().height() )
  {
    setSceneRect( QRectF( transform().dx(), transform().dy(), rect().width(), size.height() ) );
  }
  if ( painter && size.width() > rect().width() )
  {
    setSceneRect( QRectF( transform().dx(), transform().dy(), size.width(), rect().height() ) );
  }

  if ( painter )
  {
    painter->restore();

    //draw frame and selection boxes if necessary
    drawFrame( painter );
    if ( isSelected() )
    {
      drawSelectionBoxes( painter );
    }
  }

  return size;
}

QSizeF QgsComposerLegend::drawTitle( QPainter* painter, QPointF point, Qt::AlignmentFlag halignment )
{
  QSizeF size( 0, 0 );
  if ( mTitle.isEmpty() ) return size;

  QStringList lines = splitStringForWrapping( mTitle );

  double y = point.y();

  if ( painter ) painter->setPen( mFontColor );

  for ( QStringList::Iterator titlePart = lines.begin(); titlePart != lines.end(); ++titlePart )
  {
    // it does not draw the last world if rectangle width is exactly text width
    double width = textWidthMillimeters( mTitleFont, *titlePart ) + 1;
    double height = fontAscentMillimeters( mTitleFont ) + fontDescentMillimeters( mTitleFont );

    double left = halignment == Qt::AlignLeft ?  point.x() : point.x() - width / 2;

    QRectF rect( left, y, width, height );

    if ( painter ) drawText( painter, rect, *titlePart, mTitleFont, halignment, Qt::AlignVCenter );

    size.rwidth() = qMax( width, size.width() );

    y += height;
    if ( titlePart != lines.end() )
    {
      y += mlineSpacing;
    }
  }
  size.rheight() = y - point.y();

  return size;
}


QSizeF QgsComposerLegend::drawGroupItemTitle( QgsComposerGroupItem* groupItem, QPainter* painter, QPointF point )
{
  QSizeF size( 0, 0 );
  if ( !groupItem ) return size;

  double y = point.y();

  if ( painter ) painter->setPen( mFontColor );

  QStringList lines = splitStringForWrapping( groupItem->text() );
  for ( QStringList::Iterator groupPart = lines.begin(); groupPart != lines.end(); ++groupPart )
  {
    y += fontAscentMillimeters( mGroupFont );
    if ( painter ) drawText( painter, point.x(), y, *groupPart, mGroupFont );
    double width = textWidthMillimeters( mGroupFont, *groupPart );
    size.rwidth() = qMax( width, size.width() );
    if ( groupPart != lines.end() )
    {
      y += mlineSpacing;
    }
  }
  size.rheight() = y - point.y();
  return size;
}

QSizeF QgsComposerLegend::drawLayerItemTitle( QgsComposerLayerItem* layerItem, QPainter* painter, QPointF point )
{
  QSizeF size( 0, 0 );
  if ( !layerItem ) return size;

  //Let the user omit the layer title item by having an empty layer title string
  if ( layerItem->text().isEmpty() ) return size;

  double y = point.y();

  if ( painter ) painter->setPen( mFontColor );

  QStringList lines = splitStringForWrapping( layerItem->text() );
  for ( QStringList::Iterator layerItemPart = lines.begin(); layerItemPart != lines.end(); ++layerItemPart )
  {
    y += fontAscentMillimeters( mLayerFont );
    if ( painter ) drawText( painter, point.x(), y, *layerItemPart , mLayerFont );
    double width = textWidthMillimeters( mLayerFont, *layerItemPart );
    size.rwidth() = qMax( width, size.width() );
    if ( layerItemPart != lines.end() )
    {
      y += mlineSpacing;
    }
  }
  size.rheight() = y - point.y();

  return size;
}

void QgsComposerLegend::adjustBoxSize()
{
  QSizeF size = paintAndDetermineSize( 0 );
  QgsDebugMsg( QString( "width = %1 height = %2" ).arg( size.width() ).arg( size.height() ) );
  if ( size.isValid() )
  {
    setSceneRect( QRectF( transform().dx(), transform().dy(), size.width(), size.height() ) );
  }
}

QgsComposerLegend::Nucleon QgsComposerLegend::drawSymbolItem( QgsComposerLegendItem* symbolItem, QPainter* painter, QPointF point, double labelXOffset )
{
  QSizeF symbolSize( 0, 0 );
  QSizeF labelSize( 0, 0 );
  if ( !symbolItem ) return Nucleon();

  double textHeight = fontHeightCharacterMM( mItemFont, QChar( '0' ) );
  // itemHeight here is not realy item height, it is only for symbol
  // vertical alignment purpose, i.e. ok take single line height
  // if there are more lines, thos run under the symbol
  double itemHeight = qMax( mSymbolHeight, textHeight );

  //real symbol height. Can be different from standard height in case of point symbols
  double realSymbolHeight;

  int opacity = 255;
  QgsComposerLayerItem* layerItem = dynamic_cast<QgsComposerLayerItem*>( symbolItem->parent() );
  if ( layerItem )
  {
    QgsMapLayer* currentLayer = QgsMapLayerRegistry::instance()->mapLayer( layerItem->layerID() );
    if ( currentLayer )
    {
      opacity = currentLayer->getTransparency();
    }
  }

  QStringList lines = splitStringForWrapping( symbolItem->text() );

  QgsSymbol* symbol = 0;
  QgsComposerSymbolItem* symItem = dynamic_cast<QgsComposerSymbolItem*>( symbolItem );
  if ( symItem )
  {
    symbol = symItem->symbol();
  }

  QgsSymbolV2* symbolNg = 0;
  QgsComposerSymbolV2Item* symbolV2Item = dynamic_cast<QgsComposerSymbolV2Item*>( symbolItem );
  if ( symbolV2Item )
  {
    symbolNg = symbolV2Item->symbolV2();
  }
  QgsComposerRasterSymbolItem* rasterItem = dynamic_cast<QgsComposerRasterSymbolItem*>( symbolItem );

  double x = point.x();
  if ( symbol )  //item with symbol?
  {
    //draw symbol
    drawSymbol( painter, symbol, point.y() + ( itemHeight - mSymbolHeight ) / 2, x, realSymbolHeight, opacity );
    symbolSize.rwidth() =  qMax( x - point.x(), mSymbolWidth );
    symbolSize.rheight() = qMax( realSymbolHeight, mSymbolHeight );
  }
  else if ( symbolNg ) //item with symbol NG?
  {
    // must be called also with painter=0 to get real size
    drawSymbolV2( painter, symbolNg, point.y() + ( itemHeight - mSymbolHeight ) / 2, x, realSymbolHeight, opacity );
    symbolSize.rwidth() = qMax( x - point.x(), mSymbolWidth );
    symbolSize.rheight() = qMax( realSymbolHeight, mSymbolHeight );
  }
  else if ( rasterItem )
  {
    if ( painter )
    {
      painter->setBrush( rasterItem->color() );
      painter->drawRect( QRectF( point.x(), point.y() + ( itemHeight - mSymbolHeight ) / 2, mSymbolWidth, mSymbolHeight ) );
    }
    symbolSize.rwidth() = mSymbolWidth;
    symbolSize.rheight() = mSymbolHeight;
  }
  else //item with icon?
  {
    QIcon symbolIcon = symbolItem->icon();
    if ( !symbolIcon.isNull() )
    {
      if ( painter ) symbolIcon.paint( painter, point.x(), point.y() + ( itemHeight - mSymbolHeight ) / 2, mSymbolWidth, mSymbolHeight );
      symbolSize.rwidth() = mSymbolWidth;
      symbolSize.rheight() = mSymbolHeight;
    }
  }

  if ( painter ) painter->setPen( mFontColor );

  double labelX = point.x() + labelXOffset; // + mIconLabelSpace;

  // Vertical alignment of label with symbol:
  // a) label height < symbol heigh: label centerd with symbol
  // b) label height > symbol height: label starts at top and runs under symbol

  labelSize.rheight() = lines.count() * textHeight + ( lines.count() - 1 ) * mlineSpacing;

  double labelY;
  if ( labelSize.height() < symbolSize.height() )
  {
    labelY = point.y() +  symbolSize.height() / 2 + textHeight / 2;
  }
  else
  {
    labelY = point.y() + textHeight;
  }

  for ( QStringList::Iterator itemPart = lines.begin(); itemPart != lines.end(); ++itemPart )
  {
    if ( painter ) drawText( painter, labelX, labelY, *itemPart , mItemFont );
    labelSize.rwidth() = qMax( textWidthMillimeters( mItemFont,  *itemPart ), labelSize.width() );
    if ( itemPart != lines.end() )
    {
      labelY += mlineSpacing + textHeight;
    }
  }

  Nucleon nucleon;
  nucleon.item = symbolItem;
  nucleon.symbolSize = symbolSize;
  nucleon.labelSize = labelSize;
  //QgsDebugMsg( QString( "symbol height = %1 label height = %2").arg( symbolSize.height()).arg( labelSize.height() ));
  double width = symbolSize.width() + labelXOffset + labelSize.width();
  double height = qMax( symbolSize.height(), labelSize.height() );
  nucleon.size = QSizeF( width, height );
  return nucleon;
}

void QgsComposerLegend::drawSymbol( QPainter* p, QgsSymbol* s, double currentYCoord, double& currentXPosition, double& symbolHeight, int layerOpacity ) const
{
  if ( !s )
  {
    return;
  }

  QGis::GeometryType symbolType = s->type();
  switch ( symbolType )
  {
    case QGis::Point:
      drawPointSymbol( p, s, currentYCoord, currentXPosition, symbolHeight, layerOpacity );
      break;
    case QGis::Line:
      drawLineSymbol( p, s, currentYCoord, currentXPosition, layerOpacity );
      symbolHeight = mSymbolHeight;
      break;
    case QGis::Polygon:
      drawPolygonSymbol( p, s, currentYCoord, currentXPosition, layerOpacity );
      symbolHeight = mSymbolHeight;
      break;
    case QGis::UnknownGeometry:
    case QGis::NoGeometry:
      // shouldn't occur
      break;
  }
}

void QgsComposerLegend::drawSymbolV2( QPainter* p, QgsSymbolV2* s, double currentYCoord, double& currentXPosition, double& symbolHeight, int layerOpacity ) const
{
  Q_UNUSED( layerOpacity );
  if ( !s )
  {
    return;
  }

  double rasterScaleFactor = 1.0;
  if ( p )
  {
    QPaintDevice* paintDevice = p->device();
    if ( !paintDevice )
    {
      return;
    }
    rasterScaleFactor = ( paintDevice->logicalDpiX() + paintDevice->logicalDpiY() ) / 2.0 / 25.4;
  }

  //consider relation to composer map for symbol sizes in mm
  bool sizeInMapUnits = s->outputUnit() == QgsSymbolV2::MapUnit;
  double mmPerMapUnit = 1;
  if ( mComposerMap )
  {
    mmPerMapUnit = mComposerMap->mapUnitsToMM();
  }
  QgsMarkerSymbolV2* markerSymbol = dynamic_cast<QgsMarkerSymbolV2*>( s );

  //Consider symbol size for point markers
  double height = mSymbolHeight;
  double width = mSymbolWidth;
  double size = 0;
  //Center small marker symbols
  double widthOffset = 0;
  double heightOffset = 0;

  if ( markerSymbol )
  {
    size = markerSymbol->size();
    height = size;
    width = size;
    if ( mComposerMap && sizeInMapUnits )
    {
      height *= mmPerMapUnit;
      width *= mmPerMapUnit;
      markerSymbol->setSize( width );
    }
    if ( width < mSymbolWidth )
    {
      widthOffset = ( mSymbolWidth - width ) / 2.0;
    }
    if ( height < mSymbolHeight )
    {
      heightOffset = ( mSymbolHeight - height ) / 2.0;
    }
  }

  if ( p )
  {
    p->save();
    p->translate( currentXPosition + widthOffset, currentYCoord + heightOffset );
    p->scale( 1.0 / rasterScaleFactor, 1.0 / rasterScaleFactor );

    if ( markerSymbol && sizeInMapUnits )
    {
      s->setOutputUnit( QgsSymbolV2::MM );
    }

    s->drawPreviewIcon( p, QSize( width * rasterScaleFactor, height * rasterScaleFactor ) );

    if ( markerSymbol && sizeInMapUnits )
    {
      s->setOutputUnit( QgsSymbolV2::MapUnit );
      markerSymbol->setSize( size );
    }
    p->restore();
  }
  currentXPosition += width;
  currentXPosition += 2 * widthOffset;
  symbolHeight = height + 2 * heightOffset;
}

void QgsComposerLegend::drawPointSymbol( QPainter* p, QgsSymbol* s, double currentYCoord, double& currentXPosition, double& symbolHeight, int opacity ) const
{
  if ( !s )
  {
    return;
  }

  QImage pointImage;
  double rasterScaleFactor = 1.0;
  if ( p )
  {
    QPaintDevice* paintDevice = p->device();
    if ( !paintDevice )
    {
      return;
    }

    rasterScaleFactor = ( paintDevice->logicalDpiX() + paintDevice->logicalDpiY() ) / 2.0 / 25.4;
  }

  //width scale is 1.0
  pointImage = s->getPointSymbolAsImage( 1.0, false, Qt::yellow, 1.0, 0.0, rasterScaleFactor, opacity / 255.0 );

  if ( p )
  {
    p->save();
    p->scale( 1.0 / rasterScaleFactor, 1.0 / rasterScaleFactor );

    QPointF imageTopLeft( currentXPosition * rasterScaleFactor, currentYCoord * rasterScaleFactor );
    p->drawImage( imageTopLeft, pointImage );
    p->restore();
  }

  currentXPosition += s->pointSize(); //pointImage.width() / rasterScaleFactor;
  symbolHeight = s->pointSize(); //pointImage.height() / rasterScaleFactor;
}

void QgsComposerLegend::drawLineSymbol( QPainter* p, QgsSymbol* s, double currentYCoord, double& currentXPosition, int opacity ) const
{
  if ( !s )
  {
    return;
  }

  double yCoord = currentYCoord + mSymbolHeight / 2;

  if ( p )
  {
    p->save();
    QPen symbolPen = s->pen();
    QColor penColor = symbolPen.color();
    penColor.setAlpha( opacity );
    symbolPen.setColor( penColor );
    symbolPen.setCapStyle( Qt::FlatCap );
    p->setPen( symbolPen );
    p->drawLine( QPointF( currentXPosition, yCoord ), QPointF( currentXPosition + mSymbolWidth, yCoord ) );
    p->restore();
  }

  currentXPosition += mSymbolWidth;
}

void QgsComposerLegend::drawPolygonSymbol( QPainter* p, QgsSymbol* s, double currentYCoord, double& currentXPosition, int opacity ) const
{
  if ( !s )
  {
    return;
  }

  if ( p )
  {
    //scale brush and set transparencies
    QBrush symbolBrush = s->brush();
    QColor brushColor = symbolBrush.color();
    brushColor.setAlpha( opacity );
    symbolBrush.setColor( brushColor );
    QPaintDevice* paintDevice = p->device();
    if ( paintDevice )
    {
      double rasterScaleFactor = ( paintDevice->logicalDpiX() + paintDevice->logicalDpiY() ) / 2.0 / 25.4;
      QgsRenderer::scaleBrush( symbolBrush, rasterScaleFactor );
    }
    p->setBrush( symbolBrush );

    QPen symbolPen = s->pen();
    QColor penColor = symbolPen.color();
    penColor.setAlpha( opacity );
    symbolPen.setColor( penColor );
    p->setPen( symbolPen );

    p->drawRect( QRectF( currentXPosition, currentYCoord, mSymbolWidth, mSymbolHeight ) );
  }

  currentXPosition += mSymbolWidth;
}

QStringList QgsComposerLegend::layerIdList() const
{
  //take layer list from map renderer (to have legend order)
  if ( mComposition )
  {
    QgsMapRenderer* r = mComposition->mapRenderer();
    if ( r )
    {
      return r->layerSet();
    }
  }
  return QStringList();
}

void QgsComposerLegend::synchronizeWithModel()
{
  QgsDebugMsg( "Entered" );
  adjustBoxSize();
  update();
}

void QgsComposerLegend::setTitleFont( const QFont& f )
{
  mTitleFont = f;
  adjustBoxSize();
  update();
}

void QgsComposerLegend::setGroupFont( const QFont& f )
{
  mGroupFont = f;
  adjustBoxSize();
  update();
}

void QgsComposerLegend::setLayerFont( const QFont& f )
{
  mLayerFont = f;
  adjustBoxSize();
  update();
}

void QgsComposerLegend::setItemFont( const QFont& f )
{
  mItemFont = f;
  adjustBoxSize();
  update();
}

QFont QgsComposerLegend::titleFont() const
{
  return mTitleFont;
}

QFont QgsComposerLegend::groupFont() const
{
  return mGroupFont;
}

QFont QgsComposerLegend::layerFont() const
{
  return mLayerFont;
}

QFont QgsComposerLegend::itemFont() const
{
  return mItemFont;
}

void QgsComposerLegend::updateLegend()
{
  mLegendModel.setLayerSet( layerIdList() );
  adjustBoxSize();
  update();
}

bool QgsComposerLegend::writeXML( QDomElement& elem, QDomDocument & doc ) const
{
  if ( elem.isNull() )
  {
    return false;
  }

  QDomElement composerLegendElem = doc.createElement( "ComposerLegend" );

  //write general properties
  composerLegendElem.setAttribute( "title", mTitle );
  composerLegendElem.setAttribute( "columnCount", QString::number( mColumnCount ) );
  composerLegendElem.setAttribute( "splitLayer", QString::number( mSplitLayer ) );
  composerLegendElem.setAttribute( "equalColumnWidth", QString::number( mEqualColumnWidth ) );
  composerLegendElem.setAttribute( "titleFont", mTitleFont.toString() );
  composerLegendElem.setAttribute( "groupFont", mGroupFont.toString() );
  composerLegendElem.setAttribute( "layerFont", mLayerFont.toString() );
  composerLegendElem.setAttribute( "itemFont", mItemFont.toString() );
  composerLegendElem.setAttribute( "boxSpace", QString::number( mBoxSpace ) );
  composerLegendElem.setAttribute( "columnSpace", QString::number( mColumnSpace ) );
  composerLegendElem.setAttribute( "groupSpace", QString::number( mGroupSpace ) );
  composerLegendElem.setAttribute( "layerSpace", QString::number( mLayerSpace ) );
  composerLegendElem.setAttribute( "symbolSpace", QString::number( mSymbolSpace ) );
  composerLegendElem.setAttribute( "iconLabelSpace", QString::number( mIconLabelSpace ) );
  composerLegendElem.setAttribute( "symbolWidth", QString::number( mSymbolWidth ) );
  composerLegendElem.setAttribute( "symbolHeight", QString::number( mSymbolHeight ) );
  composerLegendElem.setAttribute( "wrapChar", mWrapChar );
  composerLegendElem.setAttribute( "fontColor", mFontColor.name() );

  if ( mComposerMap )
  {
    composerLegendElem.setAttribute( "map", mComposerMap->id() );
  }

  //write model properties
  mLegendModel.writeXML( composerLegendElem, doc );

  elem.appendChild( composerLegendElem );
  return _writeXML( composerLegendElem, doc );
}

bool QgsComposerLegend::readXML( const QDomElement& itemElem, const QDomDocument& doc )
{
  if ( itemElem.isNull() )
  {
    return false;
  }

  //read general properties
  mTitle = itemElem.attribute( "title" );
  mColumnCount = itemElem.attribute( "columnCount", "1" ).toInt();
  if ( mColumnCount < 1 ) mColumnCount = 1;
  mSplitLayer = itemElem.attribute( "splitLayer", "0" ).toInt() == 1;
  mEqualColumnWidth = itemElem.attribute( "equalColumnWidth", "0" ).toInt() == 1;
  //title font
  QString titleFontString = itemElem.attribute( "titleFont" );
  if ( !titleFontString.isEmpty() )
  {
    mTitleFont.fromString( titleFontString );
  }
  //group font
  QString groupFontString = itemElem.attribute( "groupFont" );
  if ( !groupFontString.isEmpty() )
  {
    mGroupFont.fromString( groupFontString );
  }

  //layer font
  QString layerFontString = itemElem.attribute( "layerFont" );
  if ( !layerFontString.isEmpty() )
  {
    mLayerFont.fromString( layerFontString );
  }
  //item font
  QString itemFontString = itemElem.attribute( "itemFont" );
  if ( !itemFontString.isEmpty() )
  {
    mItemFont.fromString( itemFontString );
  }

  //font color
  mFontColor.setNamedColor( itemElem.attribute( "fontColor", "#000000" ) );

  //spaces
  mBoxSpace = itemElem.attribute( "boxSpace", "2.0" ).toDouble();
  mColumnSpace = itemElem.attribute( "columnSpace", "2.0" ).toDouble();
  mGroupSpace = itemElem.attribute( "groupSpace", "3.0" ).toDouble();
  mLayerSpace = itemElem.attribute( "layerSpace", "3.0" ).toDouble();
  mSymbolSpace = itemElem.attribute( "symbolSpace", "2.0" ).toDouble();
  mIconLabelSpace = itemElem.attribute( "iconLabelSpace", "2.0" ).toDouble();
  mSymbolWidth = itemElem.attribute( "symbolWidth", "7.0" ).toDouble();
  mSymbolHeight = itemElem.attribute( "symbolHeight", "14.0" ).toDouble();

  mWrapChar = itemElem.attribute( "wrapChar" );

  //composer map
  if ( !itemElem.attribute( "map" ).isEmpty() )
  {
    mComposerMap = mComposition->getComposerMapById( itemElem.attribute( "map" ).toInt() );
  }

  //read model properties
  QDomNodeList modelNodeList = itemElem.elementsByTagName( "Model" );
  if ( modelNodeList.size() > 0 )
  {
    QDomElement modelElem = modelNodeList.at( 0 ).toElement();
    mLegendModel.readXML( modelElem, doc );
  }

  //restore general composer item properties
  QDomNodeList composerItemList = itemElem.elementsByTagName( "ComposerItem" );
  if ( composerItemList.size() > 0 )
  {
    QDomElement composerItemElem = composerItemList.at( 0 ).toElement();
    _readXML( composerItemElem, doc );
  }

  emit itemChanged();
  return true;
}

void QgsComposerLegend::setComposerMap( const QgsComposerMap* map )
{
  mComposerMap = map;
  if ( map )
  {
    QObject::connect( map, SIGNAL( destroyed( QObject* ) ), this, SLOT( invalidateCurrentMap() ) );
  }
}

void QgsComposerLegend::invalidateCurrentMap()
{
  if ( mComposerMap )
  {
    disconnect( mComposerMap, SIGNAL( destroyed( QObject* ) ), this, SLOT( invalidateCurrentMap() ) );
  }
  mComposerMap = 0;
}

QStringList QgsComposerLegend::splitStringForWrapping( QString stringToSplt )
{
  QStringList list;
  // If the string contains nothing then just return the string without spliting.
  if ( mWrapChar.count() == 0 )
    list << stringToSplt;
  else
    list = stringToSplt.split( mWrapChar );
  return list;
}

QList<QgsComposerLegend::Atom> QgsComposerLegend::createAtomList( QStandardItem* rootItem, bool splitLayer )
{
  QList<Atom> atoms;

  if ( !rootItem ) return atoms;

  Atom atom;

  for ( int i = 0; i < rootItem->rowCount(); i++ )
  {
    QStandardItem* currentLayerItem = rootItem->child( i );
    QgsComposerLegendItem* currentLegendItem = dynamic_cast<QgsComposerLegendItem*>( currentLayerItem );
    if ( !currentLegendItem ) continue;

    QgsComposerLegendItem::ItemType type = currentLegendItem->itemType();
    if ( type == QgsComposerLegendItem::GroupItem )
    {
      // Group subitems
      QList<Atom> groupAtoms = createAtomList( currentLayerItem, splitLayer );

      Nucleon nucleon;
      nucleon.item = currentLegendItem;
      nucleon.size = drawGroupItemTitle( dynamic_cast<QgsComposerGroupItem*>( currentLegendItem ) );

      if ( groupAtoms.size() > 0 )
      {
        // Add internal space between this group title and the next nucleon
        groupAtoms[0].size.rheight() += spaceAboveAtom( groupAtoms[0] );
        // Prepend this group title to the first atom
        groupAtoms[0].nucleons.prepend( nucleon );
        groupAtoms[0].size.rheight() += nucleon.size.height();
        groupAtoms[0].size.rwidth() = qMax( nucleon.size.width(), groupAtoms[0].size.width() );
      }
      else
      {
        // no subitems, append new atom
        Atom atom;
        atom.nucleons.append( nucleon );
        atom.size.rwidth() += nucleon.size.width();
        atom.size.rheight() += nucleon.size.height();
        atom.size.rwidth() = qMax( nucleon.size.width(), atom.size.width() );
        groupAtoms.append( atom );
      }
      atoms.append( groupAtoms );
    }
    else if ( type == QgsComposerLegendItem::LayerItem )
    {
      Nucleon nucleon;
      nucleon.item = currentLegendItem;
      nucleon.size = drawLayerItemTitle( dynamic_cast<QgsComposerLayerItem*>( currentLegendItem ) );

      QList<Atom> layerAtoms;
      Atom atom;
      atom.nucleons.append( nucleon );
      atom.size.rwidth() = nucleon.size.width();
      atom.size.rheight() = nucleon.size.height();

      for ( int j = 0; j < currentLegendItem->rowCount(); j++ )
      {
        QgsComposerLegendItem * symbolItem = dynamic_cast<QgsComposerLegendItem*>( currentLegendItem->child( j, 0 ) );
        if ( !symbolItem ) continue;

        Nucleon symbolNucleon = drawSymbolItem( symbolItem );

        if ( !mSplitLayer || j == 0 )
        {
          // append to layer atom
          // the width is not correct at this moment, we must align all symbol labels
          atom.size.rwidth() = qMax( symbolNucleon.size.width(), atom.size.width() );
          atom.size.rheight() += mSymbolSpace;
          atom.size.rheight() += symbolNucleon.size.height();
          atom.nucleons.append( symbolNucleon );
        }
        else
        {
          Atom symbolAtom;
          symbolAtom.nucleons.append( symbolNucleon );
          symbolAtom.size.rwidth() = symbolNucleon.size.width();
          symbolAtom.size.rheight() = symbolNucleon.size.height();
          layerAtoms.append( symbolAtom );
        }
      }
      layerAtoms.prepend( atom );
      atoms.append( layerAtoms );
    }
  }

  return atoms;
}

void QgsComposerLegend::drawAtom( Atom atom, QPainter* painter, QPointF point )
{
  bool first = true;
  foreach ( Nucleon nucleon, atom.nucleons )
  {
    QgsComposerLegendItem* item = nucleon.item;
    //QgsDebugMsg( "text: " + item->text() );
    if ( !item ) continue;
    QgsComposerLegendItem::ItemType type = item->itemType();
    if ( type == QgsComposerLegendItem::GroupItem )
    {
      QgsComposerGroupItem* groupItem = dynamic_cast<QgsComposerGroupItem*>( item );
      if ( !groupItem ) continue;
      if ( !first ) point.ry() += mGroupSpace;
      drawGroupItemTitle( groupItem, painter, point );
    }
    else if ( type == QgsComposerLegendItem::LayerItem )
    {
      QgsComposerLayerItem* layerItem = dynamic_cast<QgsComposerLayerItem*>( item );
      if ( !layerItem ) continue;
      if ( !first ) point.ry() += mLayerSpace;
      drawLayerItemTitle( layerItem, painter, point );
    }
    else if ( type == QgsComposerLegendItem::SymbologyItem ||
              type == QgsComposerLegendItem::SymbologyV2Item ||
              type == QgsComposerLegendItem::RasterSymbolItem )
    {
      if ( !first ) point.ry() += mSymbolSpace;
      double labelXOffset = nucleon.labelXOffset;
      drawSymbolItem( item, painter, point, labelXOffset );
    }
    point.ry() += nucleon.size.height();
    first = false;
  }
}

double QgsComposerLegend::spaceAboveAtom( Atom atom )
{
  if ( atom.nucleons.size() == 0 ) return 0;

  Nucleon nucleon = atom.nucleons.first();

  QgsComposerLegendItem* item = nucleon.item;
  if ( !item ) return 0;

  QgsComposerLegendItem::ItemType type = item->itemType();
  switch ( type )
  {
    case QgsComposerLegendItem::GroupItem:
      return mGroupSpace;
      break;
    case QgsComposerLegendItem::LayerItem:
      return mLayerSpace;
      break;
    case QgsComposerLegendItem::SymbologyV2Item:
    case QgsComposerLegendItem::RasterSymbolItem:
      return mSymbolSpace;
      break;
    default:
      break;
  }
  return 0;
}

void QgsComposerLegend::setColumns( QList<Atom>& atomList )
{
  if ( mColumnCount == 0 ) return;

  // Divide atoms to columns
  double totalHeight = 0;
  bool first = true;
  double maxAtomHeight = 0;
  foreach ( Atom atom, atomList )
  {
    if ( !first )
    {
      totalHeight += spaceAboveAtom( atom );
    }
    totalHeight += atom.size.height();
    maxAtomHeight = qMax( atom.size.height(), maxAtomHeight );
    first  = false;
  }

  // We know height of each atom and we have to split them into columns
  // minimizing max column height. It is sort of bin packing problem, NP-hard.
  // We are using simple heuristic, brute fore appeared to be to slow,
  // the number of combinations is N = n!/(k!*(n-k)!) where n = atomsCount-1
  // and k = columnsCount-1

  double avgColumnHeight = totalHeight / mColumnCount;
  int currentColumn = 0;
  int currentColumnAtomCount = 0; // number of atoms in current column
  double currentColumnHeight = 0;
  double maxColumnHeight = 0;
  double closedColumnsHeight = 0;
  first = true; // first in column
  for ( int i = 0; i < atomList.size(); i++ )
  {
    Atom atom = atomList[i];
    double currentHeight = currentColumnHeight;
    if ( !first )
    {
      currentHeight += spaceAboveAtom( atom );
    }
    currentHeight += atom.size.height();

    // Recalc average height for remaining columns including current
    avgColumnHeight = ( totalHeight - closedColumnsHeight ) / ( mColumnCount - currentColumn );
    if (( currentHeight - avgColumnHeight ) > atom.size.height() / 2 // center of current atom is over average height
        && currentColumnAtomCount > 0 // do not leave empty column
        && currentHeight > maxAtomHeight  // no sense to make smaller columns than max atom height
        && currentHeight > maxColumnHeight  // no sense to make smaller columns than max column already created
        && currentColumn < mColumnCount - 1 ) // must not exceed max number of columns
    {
      // New column
      currentColumn++;
      currentColumnAtomCount = 0;
      closedColumnsHeight += currentColumnHeight;
      currentColumnHeight = atom.size.height();
    }
    else
    {
      currentColumnHeight = currentHeight;
    }
    atomList[i].column = currentColumn;
    currentColumnAtomCount++;
    maxColumnHeight = qMax( currentColumnHeight, maxColumnHeight );

    first  = false;
  }

  // Alling labels of symbols for each layr/column to the same labelXOffset
  QMap<QString, double> maxSymbolWidth;
  for ( int i = 0; i < atomList.size(); i++ )
  {
    for ( int j = 0; j < atomList[i].nucleons.size(); j++ )
    {
      QgsComposerLegendItem* item = atomList[i].nucleons[j].item;
      if ( !item ) continue;
      QgsComposerLegendItem::ItemType type = item->itemType();
      if ( type == QgsComposerLegendItem::SymbologyItem ||
           type == QgsComposerLegendItem::SymbologyV2Item ||
           type == QgsComposerLegendItem::RasterSymbolItem )
      {
        QString key = QString( "%1-%2" ).arg(( qulonglong )item->parent() ).arg( atomList[i].column );
        maxSymbolWidth[key] = qMax( atomList[i].nucleons[j].symbolSize.width(), maxSymbolWidth[key] );
      }
    }
  }
  for ( int i = 0; i < atomList.size(); i++ )
  {
    for ( int j = 0; j < atomList[i].nucleons.size(); j++ )
    {
      QgsComposerLegendItem* item = atomList[i].nucleons[j].item;
      if ( !item ) continue;
      QgsComposerLegendItem::ItemType type = item->itemType();
      if ( type == QgsComposerLegendItem::SymbologyItem ||
           type == QgsComposerLegendItem::SymbologyV2Item ||
           type == QgsComposerLegendItem::RasterSymbolItem )
      {
        QString key = QString( "%1-%2" ).arg(( qulonglong )item->parent() ).arg( atomList[i].column );
        atomList[i].nucleons[j].labelXOffset =  maxSymbolWidth[key] + mIconLabelSpace;
        atomList[i].nucleons[j].size.rwidth() =  maxSymbolWidth[key] + mIconLabelSpace + atomList[i].nucleons[j].labelSize.width();
      }
    }
  }
}
