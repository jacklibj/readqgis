/***************************************************************************
                               qgsvectorlayer.cpp
                              --------------------
          begin                : Oct 29, 2003
          copyright            : (C) 2003 by Gary E.Sherman
          email                : sherman at mrcc.com

  This class implements a generic means to display vector layers. The features
  and attributes are read from the data store using a "data provider" plugin.
  QgsVectorLayer can be used with any data store for which an appropriate
  plugin is available.

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

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QProgressDialog>
#include <QSettings>
#include <QString>
#include <QDomNode>

#include "qgsvectorlayer.h"

// renderers
#include "qgscontinuouscolorrenderer.h"
#include "qgsgraduatedsymbolrenderer.h"
#include "qgsrenderer.h"
#include "qgssinglesymbolrenderer.h"
#include "qgsuniquevaluerenderer.h"

#include "qgsattributeaction.h"

#include "qgis.h" //for globals
#include "qgsapplication.h"
#include "qgscoordinatetransform.h"
#include "qgsfeature.h"
#include "qgsfeaturerequest.h"
#include "qgsfield.h"
#include "qgsgeometry.h"
#include "qgslabel.h"
#include "qgslogger.h"
#include "qgsmessagelog.h"
#include "qgsmaptopixel.h"
#include "qgspoint.h"
#include "qgsproviderregistry.h"
#include "qgsrectangle.h"
#include "qgsrendercontext.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayercache.h"
#include "qgsvectorlayereditbuffer.h"
#include "qgsvectorlayereditutils.h"
#include "qgsvectorlayerfeatureiterator.h"
#include "qgsvectorlayerjoinbuffer.h"
#include "qgsvectorlayerundocommand.h"
#include "qgsvectoroverlay.h"
#include "qgsmaplayerregistry.h"
#include "qgsclipper.h"
#include "qgsproject.h"

#include "qgsrendererv2.h"
#include "qgssymbolv2.h"
#include "qgssymbollayerv2.h"
#include "qgssinglesymbolrendererv2.h"
#include "qgsdiagramrendererv2.h"
#include "qgsstylev2.h"

#ifdef TESTPROVIDERLIB
#include <dlfcn.h>
#endif




QgsVectorLayer::QgsVectorLayer( QString vectorLayerPath,
                                QString baseName,
                                QString providerKey,
                                bool loadDefaultStyleFlag )
    : QgsMapLayer( VectorLayer, baseName, vectorLayerPath )
    , mUpdateThreshold( 0 )     // XXX better default value?
    , mDataProvider( NULL )
    , mProviderKey( providerKey )
    , mReadOnly( false )
    , mRenderer( 0 )
    , mRendererV2( NULL )
    , mUsingRendererV2( false )
    , mLabel( 0 )
    , mLabelOn( false )
    , mVertexMarkerOnlyForSelection( false )
    , mEditorLayout( GeneratedLayout )
    , mCache( new QgsVectorLayerCache( this ) )
    , mEditBuffer( 0 )
    , mJoinBuffer( 0 )
    , mDiagramRenderer( 0 )
    , mDiagramLayerSettings( 0 )
    , mValidExtent( false )
    , mSymbolFeatureCounted( false )
{
  mActions = new QgsAttributeAction( this );

  // if we're given a provider type, try to create and bind one to this layer
  if ( ! mProviderKey.isEmpty() )
  {
    setDataProvider( mProviderKey );
  }
  if ( mValid )
  {
    // Always set crs
    setCoordinateSystem();

    mJoinBuffer = new QgsVectorLayerJoinBuffer();

    updateFields();

    QSettings settings;
    //Changed to default to true as of QGIS 1.7
    //TODO: remove hack when http://hub.qgis.org/issues/5170 is fixed
#ifdef ANDROID
    bool use_symbology_ng_default = false;
#else
    bool use_symbology_ng_default = true;
#endif
    if ( settings.value( "/qgis/use_symbology_ng", use_symbology_ng_default ).toBool() && hasGeometryType() )
    {
      // using symbology-ng!
      setUsingRendererV2( true );
    }

    // check if there is a default style / propertysheet defined
    // for this layer and if so apply it
    bool defaultLoadedFlag = false;
    if ( loadDefaultStyleFlag )
    {
      loadDefaultStyle( defaultLoadedFlag );
    }

    // if the default style failed to load or was disabled use some very basic defaults
    if ( !defaultLoadedFlag && hasGeometryType() )
    {
      // add single symbol renderer
      if ( mUsingRendererV2 )
      {
        setRendererV2( QgsFeatureRendererV2::defaultRenderer( geometryType() ) );
      }
      else
      {
        QgsSingleSymbolRenderer *renderer = new QgsSingleSymbolRenderer( geometryType() );
        setRenderer( renderer );
      }
    }

    connect( QgsMapLayerRegistry::instance(), SIGNAL( layerWillBeRemoved( QString ) ), this, SLOT( checkJoinLayerRemove( QString ) ) );

    // Get the update threshold from user settings. We
    // do this only on construction to avoid the penality of
    // fetching this each time the layer is drawn. If the user
    // changes the threshold from the preferences dialog, it will
    // have no effect on existing layers
    // TODO: load this setting somewhere else [MD]
    //QSettings settings;
    //mUpdateThreshold = settings.readNumEntry("Map/updateThreshold", 1000);
  }
} // QgsVectorLayer ctor



QgsVectorLayer::~QgsVectorLayer()
{
  QgsDebugMsg( "entered." );

  if ( !mLayerIterator.isClosed() )
    mLayerIterator.close();

  emit layerDeleted();

  mValid = false;

  delete mRenderer;
  delete mDataProvider;
  delete mEditBuffer;
  delete mJoinBuffer;
  delete mCache;
  delete mLabel;
  delete mDiagramLayerSettings;

  delete mActions;

  //delete remaining overlays

  QList<QgsVectorOverlay*>::iterator overlayIt = mOverlays.begin();
  for ( ; overlayIt != mOverlays.end(); ++overlayIt )
  {
    delete *overlayIt;
  }
}

QString QgsVectorLayer::storageType() const
{
  if ( mDataProvider )
  {
    return mDataProvider->storageType();
  }
  return 0;
}


QString QgsVectorLayer::capabilitiesString() const
{
  if ( mDataProvider )
  {
    return mDataProvider->capabilitiesString();
  }
  return 0;
}

QString QgsVectorLayer::dataComment() const
{
  if ( mDataProvider )
  {
    return mDataProvider->dataComment();
  }
  return QString();
}


QString QgsVectorLayer::providerType() const
{
  return mProviderKey;
}

/**
 * sets the preferred display field based on some fuzzy logic
 */
void QgsVectorLayer::setDisplayField( QString fldName )
{
  if ( !hasGeometryType() )
    return;

  // If fldName is provided, use it as the display field, otherwise
  // determine the field index for the feature column of the identify
  // dialog. We look for fields containing "name" first and second for
  // fields containing "id". If neither are found, the first field
  // is used as the node.
  QString idxName = "";
  QString idxId = "";

  if ( !fldName.isEmpty() )
  {
    mDisplayField = fldName;
  }
  else
  {
    const QgsFields &fields = pendingFields();
    int fieldsSize = fields.size();

    for ( int idx = 0; idx < fields.count(); ++idx )
    {
      QString fldName = fields[idx].name();
      QgsDebugMsg( "Checking field " + fldName + " of " + QString::number( fieldsSize ) + " total" );

      // Check the fields and keep the first one that matches.
      // We assume that the user has organized the data with the
      // more "interesting" field names first. As such, name should
      // be selected before oldname, othername, etc.
      if ( fldName.indexOf( "name", false ) > -1 )
      {
        if ( idxName.isEmpty() )
        {
          idxName = fldName;
        }
      }
      if ( fldName.indexOf( "descrip", false ) > -1 )
      {
        if ( idxName.isEmpty() )
        {
          idxName = fldName;
        }
      }
      if ( fldName.indexOf( "id", false ) > -1 )
      {
        if ( idxId.isEmpty() )
        {
          idxId = fldName;
        }
      }
    }

    //if there were no fields in the dbf just return - otherwise qgis segfaults!
    if ( fieldsSize == 0 )
      return;

    if ( idxName.length() > 0 )
    {
      mDisplayField = idxName;
    }
    else
    {
      if ( idxId.length() > 0 )
      {
        mDisplayField = idxId;
      }
      else
      {
        mDisplayField = fields[0].name();
      }
    }

  }
}

// NOTE this is a temporary method added by Tim to prevent label clipping
// which was occurring when labeller was called in the main draw loop
// This method will probably be removed again in the near future!
void QgsVectorLayer::drawLabels( QgsRenderContext& rendererContext )
{
  if ( !hasGeometryType() )
    return;

  QgsDebugMsg( "Starting draw of labels: " + id() );

  if (( mRenderer || mRendererV2 ) && mLabelOn &&
      ( !mLabel->scaleBasedVisibility() ||
        ( mLabel->minScale() <= rendererContext.rendererScale() &&
          rendererContext.rendererScale() <= mLabel->maxScale() ) ) )
  {
    QgsAttributeList attributes;
    if ( mRenderer )
    {
      attributes = mRenderer->classificationAttributes();
    }
    else if ( mRendererV2 )
    {
      foreach ( QString attrName, mRendererV2->usedAttributes() )
      {
        int attrNum = fieldNameIndex( attrName );
        attributes.append( attrNum );
      }
      // make sure the renderer is ready for classification ("symbolForFeature")
      mRendererV2->startRender( rendererContext, this );
    }

    // Add fields required for labels
    mLabel->addRequiredFields( attributes );

    QgsDebugMsg( "Selecting features based on view extent" );

    int featureCount = 0;

    try
    {
      // select the records in the extent. The provider sets a spatial filter
      // and sets up the selection set for retrieval
      QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                            .setFilterRect( rendererContext.extent() )
                                            .setSubsetOfAttributes( attributes ) );

      QgsFeature fet;
      while ( fit.nextFeature( fet ) )
      {
        if (( mRenderer && mRenderer->willRenderFeature( &fet ) )
            || ( mRendererV2 && mRendererV2->willRenderFeature( fet ) ) )
        {
          bool sel = mSelectedFeatureIds.contains( fet.id() );
          mLabel->renderLabel( rendererContext, fet, sel, 0 );
        }
        featureCount++;
      }
    }
    catch ( QgsCsException &e )
    {
      Q_UNUSED( e );
      QgsDebugMsg( "Error projecting label locations" );
    }

    if ( mRendererV2 )
    {
      mRendererV2->stopRender( rendererContext );
    }

    QgsDebugMsg( QString( "Total features processed %1" ).arg( featureCount ) );

    // XXX Something in our draw event is triggering an additional draw event when resizing [TE 01/26/06]
    // XXX Calling this will begin processing the next draw event causing image havoc and recursion crashes.
    //qApp->processEvents();

  }
}


unsigned char *QgsVectorLayer::drawLineString( unsigned char *feature, QgsRenderContext &renderContext )
{
  QPainter *p = renderContext.painter();
  unsigned char *ptr = feature + 5;
  unsigned int wkbType = *(( int* )( feature + 1 ) );
  int nPoints = *(( int* )ptr );
  ptr = feature + 9;

  bool hasZValue = ( wkbType == QGis::WKBLineString25D );

  QVector<double> x( nPoints );
  QVector<double> y( nPoints );
  QVector<double> z( nPoints, 0.0 );

  // Extract the points from the WKB format into the x and y vectors.
  for ( int i = 0; i < nPoints; ++i )
  {
    x[i] = *(( double * ) ptr );
    ptr += sizeof( double );
    y[i] = *(( double * ) ptr );
    ptr += sizeof( double );

    if ( hasZValue ) // ignore Z value
      ptr += sizeof( double );
  }

  // Transform the points into map coordinates (and reproject if
  // necessary)

  transformPoints( x, y, z, renderContext );

  // Work around a +/- 32768 limitation on coordinates
  // Look through the x and y coordinates and see if there are any
  // that need trimming. If one is found, there's no need to look at
  // the rest of them so end the loop at that point.
  for ( int i = 0; i < nPoints; ++i )
  {
    if ( qAbs( x.at( i ) ) > QgsClipper::MAX_X ||
         qAbs( y.at( i ) ) > QgsClipper::MAX_Y )
    {
      QgsClipper::trimFeature( x, y, true ); // true = polyline
      nPoints = x.size(); // trimming may change nPoints.
      break;
    }
  }

  // set up QPolygonF class with transformed points
  QPolygonF pa( nPoints );
  for ( int i = 0; i < nPoints; ++i )
  {
    pa[i].setX( x.at( i ) );
    pa[i].setY( y.at( i ) );
  }

  // The default pen gives bevelled joins between segements of the
  // polyline, which is good enough for the moment.
  //preserve a copy of the pen before we start fiddling with it
  QPen pen = p->pen(); // to be kept original

  //
  // experimental alpha transparency
  // 255 = opaque
  //
  QPen myTransparentPen = p->pen(); // store current pen
  QColor myColor = myTransparentPen.color();
  //only set transparency from layer level if renderer does not provide
  //transparency on class level
  if ( !mRenderer->usesTransparency() )
  {
    myColor.setAlpha( mTransparencyLevel );
  }
  myTransparentPen.setColor( myColor );
  p->setPen( myTransparentPen );
  p->drawPolyline( pa );

  // draw vertex markers if in editing mode, but only to the main canvas
  if ( mEditBuffer && renderContext.drawEditingInformation() )
  {

    QVector<double>::const_iterator xIt;
    QVector<double>::const_iterator yIt;
    for ( xIt = x.begin(), yIt = y.begin(); xIt != x.end(); ++xIt, ++yIt )
    {
      drawVertexMarker( *xIt, *yIt, *p, mCurrentVertexMarkerType, mCurrentVertexMarkerSize );
    }
  }

  //restore the pen
  p->setPen( pen );

  return ptr;
}

unsigned char *QgsVectorLayer::drawPolygon( unsigned char *feature, QgsRenderContext &renderContext )
{
  QPainter *p = renderContext.painter();
  typedef QPair<QVector<double>, QVector<double> > ringType;
  typedef ringType* ringTypePtr;
  typedef QVector<ringTypePtr> ringsType;

  // get number of rings in the polygon
  unsigned int numRings = *(( int* )( feature + 1 + sizeof( int ) ) );

  if ( numRings == 0 )  // sanity check for zero rings in polygon
    return feature + 9;

  unsigned int wkbType = *(( int* )( feature + 1 ) );

  bool hasZValue = ( wkbType == QGis::WKBPolygon25D );

  int total_points = 0;

  // A vector containing a pointer to a pair of double vectors.The
  // first vector in the pair contains the x coordinates, and the
  // second the y coordinates.
  ringsType rings;

  // Set pointer to the first ring
  unsigned char* ptr = feature + 1 + 2 * sizeof( int );

  for ( register unsigned int idx = 0; idx < numRings; idx++ )
  {
    int nPoints = *(( int* )ptr );

    ringTypePtr ring = new ringType( QVector<double>( nPoints ), QVector<double>( nPoints ) );
    ptr += 4;

    // create a dummy vector for the z coordinate
    QVector<double> zVector( nPoints, 0.0 );
    // Extract the points from the WKB and store in a pair of
    // vectors.
    for ( int jdx = 0; jdx < nPoints; jdx++ )
    {
      ring->first[jdx] = *(( double * ) ptr );
      ptr += sizeof( double );
      ring->second[jdx] = *(( double * ) ptr );
      ptr += sizeof( double );

      if ( hasZValue )
        ptr += sizeof( double );
    }
    // If ring has fewer than two points, what is it then?
    // Anyway, this check prevents a crash
    if ( nPoints < 1 )
    {
      QgsDebugMsg( "Ring has only " + QString::number( nPoints ) + " points! Skipping this ring." );
      continue;
    }

    transformPoints( ring->first, ring->second, zVector, renderContext );

    // Work around a +/- 32768 limitation on coordinates
    // Look through the x and y coordinates and see if there are any
    // that need trimming. If one is found, there's no need to look at
    // the rest of them so end the loop at that point.
    for ( int i = 0; i < nPoints; ++i )
    {
      if ( qAbs( ring->first[i] ) > QgsClipper::MAX_X ||
           qAbs( ring->second[i] ) > QgsClipper::MAX_Y )
      {
        QgsClipper::trimFeature( ring->first, ring->second, false );
        break;
      }
    }

    // Don't bother keeping the ring if it has been trimmed out of
    // existence.
    if ( ring->first.size() == 0 )
      delete ring;
    else
    {
      rings.push_back( ring );
      total_points += ring->first.size();
    }
  }

  // Now we draw the polygons

  // use painter paths for drawing polygons with holes
  // when adding polygon to the path they invert the area
  // this means that adding inner rings to the path creates
  // holes in outer ring
  QPainterPath path; // OddEven fill rule by default

  // Only try to draw polygons if there is something to draw
  if ( total_points > 0 )
  {
    //preserve a copy of the brush and pen before we start fiddling with it
    QBrush brush = p->brush(); //to be kept as original
    QPen pen = p->pen(); // to be kept original
    //
    // experimental alpha transparency
    // 255 = opaque
    //
    QBrush myTransparentBrush = p->brush();
    QColor myColor = brush.color();

    //only set transparency from layer level if renderer does not provide
    //transparency on class level
    if ( !mRenderer->usesTransparency() )
    {
      myColor.setAlpha( mTransparencyLevel );
    }
    myTransparentBrush.setColor( myColor );
    QPen myTransparentPen = p->pen(); // store current pen
    myColor = myTransparentPen.color();

    //only set transparency from layer level if renderer does not provide
    //transparency on class level
    if ( !mRenderer->usesTransparency() )
    {
      myColor.setAlpha( mTransparencyLevel );
    }
    myTransparentPen.setColor( myColor );

    p->setBrush( myTransparentBrush );
    p->setPen( myTransparentPen );

    if ( numRings == 1 )
    {
      ringTypePtr r = rings[0];
      unsigned ringSize = r->first.size();

      QPolygonF pa( ringSize );
      for ( register unsigned int j = 0; j != ringSize; ++j )
      {
        pa[j].setX( r->first[j] );
        pa[j].setY( r->second[j] );
      }
      p->drawPolygon( pa );

      // draw vertex markers if in editing mode, but only to the main canvas
      if ( mEditBuffer && renderContext.drawEditingInformation() )
      {
        for ( register unsigned int j = 0; j != ringSize; ++j )
        {
          drawVertexMarker( r->first[j], r->second[j], *p, mCurrentVertexMarkerType, mCurrentVertexMarkerSize );
        }
      }

      delete rings[0];
    }
    else
    {
      // Store size here and use it in the loop to avoid penalty of
      // multiple calls to size()
      int numRings = rings.size();
      for ( register int i = 0; i < numRings; ++i )
      {
        // Store the pointer in a variable with a short name so as to make
        // the following code easier to type and read.
        ringTypePtr r = rings[i];
        // only do this once to avoid penalty of additional calls
        unsigned ringSize = r->first.size();

        // Transfer points to the array of QPointF
        QPolygonF pa( ringSize );
        for ( register unsigned int j = 0; j != ringSize; ++j )
        {
          pa[j].setX( r->first[j] );
          pa[j].setY( r->second[j] );
        }

        path.addPolygon( pa );

        // Tidy up the pointed to pairs of vectors as we finish with them
        delete rings[i];
      }

#if 0
      // A bit of code to aid in working out what values of
      // QgsClipper::minX, etc cause the X11 zoom bug.
      int largestX  = -std::numeric_limits<int>::max();
      int smallestX = std::numeric_limits<int>::max();
      int largestY  = -std::numeric_limits<int>::max();
      int smallestY = std::numeric_limits<int>::max();

      for ( int i = 0; i < pa.size(); ++i )
      {
        largestX  = qMax( largestX,  pa.point( i ).x() );
        smallestX = qMin( smallestX, pa.point( i ).x() );
        largestY  = qMax( largestY,  pa.point( i ).y() );
        smallestY = qMin( smallestY, pa.point( i ).y() );
      }
      QgsDebugMsg( QString( "Largest  X coordinate was %1" ).arg( largestX ) );
      QgsDebugMsg( QString( "Smallest X coordinate was %1" ).arg( smallestX ) );
      QgsDebugMsg( QString( "Largest  Y coordinate was %1" ).arg( largestY ) );
      QgsDebugMsg( QString( "Smallest Y coordinate was %1" ).arg( smallestY ) );
#endif

      //
      // draw the polygon
      //
      p->drawPath( path );

      // draw vertex markers if in editing mode, but only to the main canvas
      if ( mEditBuffer && renderContext.drawEditingInformation() )
      {
        for ( int i = 0; i < path.elementCount(); ++i )
        {
          const QPainterPath::Element & e = path.elementAt( i );
          drawVertexMarker( e.x, e.y, *p, mCurrentVertexMarkerType, mCurrentVertexMarkerSize );
        }
      }
    }

    //
    //restore brush and pen to original
    //
    p->setBrush( brush );
    p->setPen( pen );

  } // totalPoints > 0

  return ptr;
}

void QgsVectorLayer::drawRendererV2( QgsFeatureIterator &fit, QgsRenderContext& rendererContext, bool labeling )
{
  if ( !hasGeometryType() )
    return;

  QSettings settings;
  bool vertexMarkerOnlyForSelection = settings.value( "/qgis/digitizing/marker_only_for_selected", false ).toBool();

  mRendererV2->startRender( rendererContext, this );

#ifndef Q_WS_MAC
  int featureCount = 0;
#endif //Q_WS_MAC

  QgsFeature fet;
  while ( fit.nextFeature( fet ) )
  {
    try
    {
      if ( !fet.geometry() )
        continue; // skip features without geometry

      if ( rendererContext.renderingStopped() )
      {
        break;
      }
#ifndef Q_WS_MAC //MH: disable this on Mac for now to avoid problems with resizing
#ifdef Q_WS_X11
      if ( !mEnableBackbuffer ) // do not handle events, as we're already inside a paint event
      {
#endif // Q_WS_X11
        if ( mUpdateThreshold > 0 && 0 == featureCount % mUpdateThreshold )
        {
          emit screenUpdateRequested();
          // emit drawingProgress( featureCount, totalFeatures );
          qApp->processEvents();
        }
        else if ( featureCount % 1000 == 0 )
        {
          // emit drawingProgress( featureCount, totalFeatures );
          qApp->processEvents();
        }
#ifdef Q_WS_X11
      }
#endif // Q_WS_X11
#endif // Q_WS_MAC

      bool sel = mSelectedFeatureIds.contains( fet.id() );
      bool drawMarker = ( mEditBuffer && ( !vertexMarkerOnlyForSelection || sel ) );

      // render feature
      bool rendered = mRendererV2->renderFeature( fet, rendererContext, -1, sel, drawMarker );

      if ( mEditBuffer )
      {
        // Cache this for the use of (e.g.) modifying the feature's uncommitted geometry.
        mCache->cacheGeometry( fet.id(), *fet.geometry() );
      }

      // labeling - register feature
      if ( rendered && rendererContext.labelingEngine() )
      {
        if ( labeling )
        {
          rendererContext.labelingEngine()->registerFeature( this, fet, rendererContext );
        }
        if ( mDiagramRenderer )
        {
          rendererContext.labelingEngine()->registerDiagramFeature( this, fet, rendererContext );
        }
      }
    }
    catch ( const QgsCsException &cse )
    {
      Q_UNUSED( cse );
      QgsDebugMsg( QString( "Failed to transform a point while drawing a feature with ID '%1'. Ignoring this feature. %2" )
                   .arg( fet.id() ).arg( cse.what() ) );
    }
#ifndef Q_WS_MAC
    ++featureCount;
#endif //Q_WS_MAC
  }

  stopRendererV2( rendererContext, NULL );

#ifndef Q_WS_MAC
  QgsDebugMsg( QString( "Total features processed %1" ).arg( featureCount ) );
#endif
}

void QgsVectorLayer::drawRendererV2Levels( QgsFeatureIterator &fit, QgsRenderContext& rendererContext, bool labeling )
{
  if ( !hasGeometryType() )
    return;

  QHash< QgsSymbolV2*, QList<QgsFeature> > features; // key = symbol, value = array of features

  QSettings settings;
  bool vertexMarkerOnlyForSelection = settings.value( "/qgis/digitizing/marker_only_for_selected", false ).toBool();

  // startRender must be called before symbolForFeature() calls to make sure renderer is ready
  mRendererV2->startRender( rendererContext, this );

  QgsSingleSymbolRendererV2* selRenderer = NULL;
  if ( !mSelectedFeatureIds.isEmpty() )
  {
    selRenderer = new QgsSingleSymbolRendererV2( QgsSymbolV2::defaultSymbol( geometryType() ) );
    selRenderer->symbol()->setColor( QgsRenderer::selectionColor() );
    selRenderer->setVertexMarkerAppearance( currentVertexMarkerType(), currentVertexMarkerSize() );
    selRenderer->startRender( rendererContext, this );
  }

  // 1. fetch features
  QgsFeature fet;
#ifndef Q_WS_MAC
  int featureCount = 0;
#endif //Q_WS_MAC
  while ( fit.nextFeature( fet ) )
  {
    if ( !fet.geometry() )
      continue; // skip features without geometry

    if ( rendererContext.renderingStopped() )
    {
      stopRendererV2( rendererContext, selRenderer );
      return;
    }
#ifndef Q_WS_MAC
    if ( featureCount % 1000 == 0 )
    {
      qApp->processEvents();
    }
#endif //Q_WS_MAC
    QgsSymbolV2* sym = mRendererV2->symbolForFeature( fet );
    if ( !sym )
    {
      continue;
    }

    if ( !features.contains( sym ) )
    {
      features.insert( sym, QList<QgsFeature>() );
    }
    features[sym].append( fet );

    if ( mEditBuffer )
    {
      // Cache this for the use of (e.g.) modifying the feature's uncommitted geometry.
      mCache->cacheGeometry( fet.id(), *fet.geometry() );
    }

    if ( sym && rendererContext.labelingEngine() )
    {
      if ( labeling )
      {
        rendererContext.labelingEngine()->registerFeature( this, fet, rendererContext );
      }
      if ( mDiagramRenderer )
      {
        rendererContext.labelingEngine()->registerDiagramFeature( this, fet, rendererContext );
      }
    }

#ifndef Q_WS_MAC
    ++featureCount;
#endif //Q_WS_MAC
  }

  // find out the order
  QgsSymbolV2LevelOrder levels;
  QgsSymbolV2List symbols = mRendererV2->symbols();
  for ( int i = 0; i < symbols.count(); i++ )
  {
    QgsSymbolV2* sym = symbols[i];
    for ( int j = 0; j < sym->symbolLayerCount(); j++ )
    {
      int level = sym->symbolLayer( j )->renderingPass();
      if ( level < 0 || level >= 1000 ) // ignore invalid levels
        continue;
      QgsSymbolV2LevelItem item( sym, j );
      while ( level >= levels.count() ) // append new empty levels
        levels.append( QgsSymbolV2Level() );
      levels[level].append( item );
    }
  }

  // 2. draw features in correct order
  for ( int l = 0; l < levels.count(); l++ )
  {
    QgsSymbolV2Level& level = levels[l];
    for ( int i = 0; i < level.count(); i++ )
    {
      QgsSymbolV2LevelItem& item = level[i];
      if ( !features.contains( item.symbol() ) )
      {
        QgsDebugMsg( "level item's symbol not found!" );
        continue;
      }
      int layer = item.layer();
      QList<QgsFeature>& lst = features[item.symbol()];
      QList<QgsFeature>::iterator fit;
#ifndef Q_WS_MAC
      featureCount = 0;
#endif //Q_WS_MAC
      for ( fit = lst.begin(); fit != lst.end(); ++fit )
      {
        if ( rendererContext.renderingStopped() )
        {
          stopRendererV2( rendererContext, selRenderer );
          return;
        }
#ifndef Q_WS_MAC
        if ( featureCount % 1000 == 0 )
        {
          qApp->processEvents();
        }
#endif //Q_WS_MAC
        bool sel = mSelectedFeatureIds.contains( fit->id() );
        // maybe vertex markers should be drawn only during the last pass...
        bool drawMarker = ( mEditBuffer && ( !vertexMarkerOnlyForSelection || sel ) );

        try
        {
          mRendererV2->renderFeature( *fit, rendererContext, layer, sel, drawMarker );
        }
        catch ( const QgsCsException &cse )
        {
          Q_UNUSED( cse );
          QgsDebugMsg( QString( "Failed to transform a point while drawing a feature with ID '%1'. Ignoring this feature. %2" )
                       .arg( fet.id() ).arg( cse.what() ) );
        }
#ifndef Q_WS_MAC
        ++featureCount;
#endif //Q_WS_MAC
      }
    }
  }

  stopRendererV2( rendererContext, selRenderer );
}

void QgsVectorLayer::reload()
{
  if ( mDataProvider )
  {
    mDataProvider->reloadData();
  }
}

bool QgsVectorLayer::draw( QgsRenderContext& rendererContext )
{
  if ( !hasGeometryType() )
    return true;

  //set update threshold before each draw to make sure the current setting is picked up
  QSettings settings;
  mUpdateThreshold = settings.value( "Map/updateThreshold", 0 ).toInt();
#ifdef Q_WS_X11
  mEnableBackbuffer = settings.value( "/Map/enableBackbuffer", 1 ).toBool();
#endif

  if ( mUsingRendererV2 )
  {
    if ( !mRendererV2 )
      return false;

    QgsDebugMsg( "rendering v2:\n" + mRendererV2->dump() );

    if ( mEditBuffer )
    {
      // Destroy all cached geometries and clear the references to them
      mCache->deleteCachedGeometries();
      mCache->setCachedGeometriesRect( rendererContext.extent() );

      // set editing vertex markers style
      mRendererV2->setVertexMarkerAppearance( currentVertexMarkerType(), currentVertexMarkerSize() );
    }

    QgsAttributeList attributes;
    foreach ( QString attrName, mRendererV2->usedAttributes() )
    {
      int attrNum = fieldNameIndex( attrName );
      attributes.append( attrNum );
      QgsDebugMsg( "attrs: " + attrName + " - " + QString::number( attrNum ) );
    }

    bool labeling = false;
    //register label and diagram layer to the labeling engine
    prepareLabelingAndDiagrams( rendererContext, attributes, labeling );

    QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                          .setFilterRect( rendererContext.extent() )
                                          .setSubsetOfAttributes( attributes ) );

    if (( mRendererV2->capabilities() & QgsFeatureRendererV2::SymbolLevels )
        && mRendererV2->usingSymbolLevels() )
      drawRendererV2Levels( fit, rendererContext, labeling );
    else
      drawRendererV2( fit, rendererContext, labeling );

    return true;
  }

  //draw ( p, viewExtent, theMapToPixelTransform, ct, drawingToEditingCanvas, 1., 1.);

  if ( mRenderer )
  {
    // painter is active (begin has been called
    /* Steps to draw the layer
       1. get the features in the view extent by SQL query
       2. read WKB for a feature
       3. transform
       4. draw
    */

    QPen pen;
    /*Pointer to a marker image*/
    QImage marker;
    //vertex marker type for selection
    QgsVectorLayer::VertexMarkerType vertexMarker = QgsVectorLayer::NoMarker;
    int vertexMarkerSize = 7;

    if ( mEditBuffer )
    {
      // Destroy all cached geometries and clear the references to them
      mCache->deleteCachedGeometries();
      mCache->setCachedGeometriesRect( rendererContext.extent() );
      vertexMarker = currentVertexMarkerType();
      vertexMarkerSize = currentVertexMarkerSize();
      mVertexMarkerOnlyForSelection = settings.value( "/qgis/digitizing/marker_only_for_selected", false ).toBool();
    }

    // int totalFeatures = pendingFeatureCount();
    int featureCount = 0;
    QgsFeature fet;
    QgsAttributeList attributes = mRenderer->classificationAttributes();

    bool labeling = false;
    prepareLabelingAndDiagrams( rendererContext, attributes, labeling );

    try
    {
      QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                            .setFilterRect( rendererContext.extent() )
                                            .setSubsetOfAttributes( attributes ) );
      while ( fit.nextFeature( fet ) )
      {
        if ( !fet.geometry() )
          continue; // skip features without geometry

        if ( rendererContext.renderingStopped() )
        {
          break;
        }

#ifndef Q_WS_MAC //MH: disable this on Mac for now to avoid problems with resizing
        if ( mUpdateThreshold > 0 && 0 == featureCount % mUpdateThreshold )
        {
          emit screenUpdateRequested();
          // emit drawingProgress( featureCount, totalFeatures );
          qApp->processEvents();
        }
        else if ( featureCount % 1000 == 0 )
        {
          // emit drawingProgress( featureCount, totalFeatures );
          qApp->processEvents();
        }
// #else
//         Q_UNUSED( totalFeatures );
#endif //Q_WS_MAC

        // check if feature is selected
        // only show selections of the current layer
        // TODO: create a mechanism to let layer know whether it's current layer or not [MD]
        bool sel = mSelectedFeatureIds.contains( fet.id() );

        mCurrentVertexMarkerType = QgsVectorLayer::NoMarker;
        mCurrentVertexMarkerSize = 7;

        if ( mEditBuffer )
        {
          // Cache this for the use of (e.g.) modifying the feature's uncommitted geometry.
          mCache->cacheGeometry( fet.id(), *fet.geometry() );

          if ( !mVertexMarkerOnlyForSelection || sel )
          {
            mCurrentVertexMarkerType = vertexMarker;
            mCurrentVertexMarkerSize = vertexMarkerSize;
          }
        }

        //QgsDebugMsg(QString("markerScale before renderFeature(): %1").arg(markerScaleFactor));
        // markerScalerFactore reflects the wanted scaling of the marker

        double opacity = 1.0;
        if ( !mRenderer->usesTransparency() )
        {
          opacity = ( mTransparencyLevel * 1.0 ) / 255.0;
        }
        mRenderer->renderFeature( rendererContext, fet, &marker, sel, opacity );

        // markerScalerFactore now reflects the actual scaling of the marker that the render performed.
        //QgsDebugMsg(QString("markerScale after renderFeature(): %1").arg(markerScaleFactor));

        //double scale = rendererContext.scaleFactor() /  markerScaleFactor;
        drawFeature( rendererContext, fet, &marker );

        if ( mRenderer->willRenderFeature( &fet ) && rendererContext.labelingEngine() )
        {
          if ( labeling )
          {
            rendererContext.labelingEngine()->registerFeature( this, fet, rendererContext );
          }
          if ( mDiagramRenderer )
          {
            rendererContext.labelingEngine()->registerDiagramFeature( this, fet, rendererContext );
          }
        }
        ++featureCount;
      }
    }
    catch ( QgsCsException &cse )
    {
      Q_UNUSED( cse );
      QgsDebugMsg( QString( "Failed to transform a point while drawing a feature with ID '%1'. Rendering stopped. %2" )
                   .arg( fet.id() ).arg( cse.what() ) );
      return false;
    }

    QgsDebugMsg( QString( "Total features processed %1" ).arg( featureCount ) );
  }
  else
  {
    QgsDebugMsg( "QgsRenderer is null" );
  }

  if ( mEditBuffer )
  {
    QgsDebugMsg( QString( "Cached %1 geometries." ).arg( mCache->cachedGeometries().count() ) );
  }

  return true; // Assume success always
}

void QgsVectorLayer::drawVertexMarker( double x, double y, QPainter& p, QgsVectorLayer::VertexMarkerType type, int m )
{
  if ( type == QgsVectorLayer::SemiTransparentCircle )
  {
    p.setPen( QColor( 50, 100, 120, 200 ) );
    p.setBrush( QColor( 200, 200, 210, 120 ) );
    p.drawEllipse( x - m, y - m, m * 2 + 1, m * 2 + 1 );
  }
  else if ( type == QgsVectorLayer::Cross )
  {
    p.setPen( QColor( 255, 0, 0 ) );
    p.drawLine( x - m, y + m, x + m, y - m );
    p.drawLine( x - m, y - m, x + m, y + m );
  }
}

void QgsVectorLayer::select( QgsFeatureId fid, bool emitSignal )
{
  mSelectedFeatureIds.insert( fid );

  if ( emitSignal )
  {
    // invalidate cache
    setCacheImage( 0 );

    emit selectionChanged();
  }
}

void QgsVectorLayer::deselect( QgsFeatureId fid, bool emitSignal )
{
  mSelectedFeatureIds.remove( fid );

  if ( emitSignal )
  {
    // invalidate cache
    setCacheImage( 0 );

    emit selectionChanged();
  }
}

void QgsVectorLayer::select( QgsRectangle & rect, bool lock )
{
  // normalize the rectangle
  rect.normalize();

  if ( !lock )
  {
    removeSelection( false ); // don't emit signal
  }

  //select all the elements
  QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                        .setFilterRect( rect )
                                        .setFlags( QgsFeatureRequest::ExactIntersect | QgsFeatureRequest::NoGeometry )
                                        .setSubsetOfAttributes( QgsAttributeList() ) );

  QgsFeature f;
  while ( fit.nextFeature( f ) )
  {
    select( f.id(), false ); // don't emit signal (not to redraw it everytime)
  }

  // invalidate cache
  setCacheImage( 0 );

  emit selectionChanged(); // now emit signal to redraw layer
}

void QgsVectorLayer::invertSelection()
{
  // copy the ids of selected features to tmp
  QgsFeatureIds tmp = mSelectedFeatureIds;

  removeSelection( false ); // don't emit signal

  QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                        .setFlags( QgsFeatureRequest::NoGeometry )
                                        .setSubsetOfAttributes( QgsAttributeList() ) );

  QgsFeature fet;
  while ( fit.nextFeature( fet ) )
  {
    select( fet.id(), false ); // don't emit signal
  }

  for ( QgsFeatureIds::iterator iter = tmp.begin(); iter != tmp.end(); ++iter )
  {
    mSelectedFeatureIds.remove( *iter );
  }

  // invalidate cache
  setCacheImage( 0 );

  emit selectionChanged();
}

void QgsVectorLayer::invertSelectionInRectangle( QgsRectangle &rect )
{
  // normalize the rectangle
  rect.normalize();

  QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                        .setFilterRect( rect )
                                        .setFlags( QgsFeatureRequest::NoGeometry | QgsFeatureRequest::ExactIntersect )
                                        .setSubsetOfAttributes( QgsAttributeList() ) );

  QgsFeature fet;
  while ( fit.nextFeature( fet ) )
  {
    if ( mSelectedFeatureIds.contains( fet.id() ) )
    {
      deselect( fet.id(), false ); // don't emit signal
    }
    else
    {
      select( fet.id(), false ); // don't emit signal
    }
  }

  // invalidate cache
  setCacheImage( 0 );

  emit selectionChanged();
}

void QgsVectorLayer::removeSelection( bool emitSignal )
{
  if ( mSelectedFeatureIds.size() == 0 )
    return;

  mSelectedFeatureIds.clear();

  if ( emitSignal )
  {
    // invalidate cache
    setCacheImage( 0 );

    emit selectionChanged();
  }
}

void QgsVectorLayer::triggerRepaint()
{
  emit repaintRequested();
}

QgsVectorDataProvider* QgsVectorLayer::dataProvider()
{
  return mDataProvider;
}

const QgsVectorDataProvider* QgsVectorLayer::dataProvider() const
{
  return mDataProvider;
}

void QgsVectorLayer::setProviderEncoding( const QString& encoding )
{
  if ( mDataProvider )
  {
    mDataProvider->setEncoding( encoding );
  }
}


const QgsRenderer* QgsVectorLayer::renderer() const
{
  return mRenderer;
}

void QgsVectorLayer::setRenderer( QgsRenderer *r )
{
  if ( !hasGeometryType() )
    return;

  if ( r != mRenderer )
  {
    if ( r )
      setUsingRendererV2( false );
    delete mRenderer;
    mRenderer = r;
  }
}

void QgsVectorLayer::setDiagramRenderer( QgsDiagramRendererV2* r )
{
  delete mDiagramRenderer;
  mDiagramRenderer = r;
}

QGis::GeometryType QgsVectorLayer::geometryType() const
{
  if ( mDataProvider )
  {
    int type = mDataProvider->geometryType();
    switch ( type )
    {
      case QGis::WKBPoint:
      case QGis::WKBPoint25D:
        return QGis::Point;

      case QGis::WKBLineString:
      case QGis::WKBLineString25D:
        return QGis::Line;

      case QGis::WKBPolygon:
      case QGis::WKBPolygon25D:
        return QGis::Polygon;

      case QGis::WKBMultiPoint:
      case QGis::WKBMultiPoint25D:
        return QGis::Point;

      case QGis::WKBMultiLineString:
      case QGis::WKBMultiLineString25D:
        return QGis::Line;

      case QGis::WKBMultiPolygon:
      case QGis::WKBMultiPolygon25D:
        return QGis::Polygon;

      case QGis::WKBNoGeometry:
        return QGis::NoGeometry;
    }
    QgsDebugMsg( QString( "Data Provider Geometry type is not recognised, is %1" ).arg( type ) );
  }
  else
  {
    QgsDebugMsg( "pointer to mDataProvider is null" );
  }

  // We shouldn't get here, and if we have, other things are likely to
  // go wrong. Code that uses the type() return value should be
  // rewritten to cope with a value of QGis::Unknown. To make this
  // need known, the following message is printed every time we get
  // here.
  QgsDebugMsg( "WARNING: This code should never be reached. Problems may occur..." );

  return QGis::UnknownGeometry;
}

bool QgsVectorLayer::hasGeometryType() const
{
  QGis::GeometryType t = geometryType();
  return ( t != QGis::NoGeometry && t != QGis::UnknownGeometry );
}

QGis::WkbType QgsVectorLayer::wkbType() const
{
  return ( QGis::WkbType )( mWkbType );
}

QgsRectangle QgsVectorLayer::boundingBoxOfSelected()
{
  if ( mSelectedFeatureIds.size() == 0 ) //no selected features
  {
    return QgsRectangle( 0, 0, 0, 0 );
  }

  QgsRectangle r, retval;
  retval.setMinimal();

  QgsFeature fet;
  if ( mDataProvider->capabilities() & QgsVectorDataProvider::SelectAtId )
  {
    foreach ( QgsFeatureId fid, mSelectedFeatureIds )
    {
      if ( getFeatures( QgsFeatureRequest()
                        .setFilterFid( fid )
                        .setSubsetOfAttributes( QgsAttributeList() ) )
           .nextFeature( fet ) &&
           fet.geometry() )
      {
        r = fet.geometry()->boundingBox();
        retval.combineExtentWith( &r );
      }
    }
  }
  else
  {
    QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                          .setSubsetOfAttributes( QgsAttributeList() ) );

    while ( fit.nextFeature( fet ) )
    {
      if ( mSelectedFeatureIds.contains( fet.id() ) )
      {
        if ( fet.geometry() )
        {
          r = fet.geometry()->boundingBox();
          retval.combineExtentWith( &r );
        }
      }
    }
  }

  if ( retval.width() == 0.0 || retval.height() == 0.0 )
  {
    // If all of the features are at the one point, buffer the
    // rectangle a bit. If they are all at zero, do something a bit
    // more crude.

    if ( retval.xMinimum() == 0.0 && retval.xMaximum() == 0.0 &&
         retval.yMinimum() == 0.0 && retval.yMaximum() == 0.0 )
    {
      retval.set( -1.0, -1.0, 1.0, 1.0 );
    }
  }

  return retval;
}

long QgsVectorLayer::featureCount() const
{
  if ( !mDataProvider )
  {
    QgsDebugMsg( "invoked with null mDataProvider" );
    return 0;
  }

  return mDataProvider->featureCount();
}

long QgsVectorLayer::featureCount( QgsSymbolV2* symbol )
{
  if ( !mSymbolFeatureCounted ) return -1;
  return mSymbolFeatureCountMap.value( symbol );
}

bool QgsVectorLayer::countSymbolFeatures( bool showProgress )
{
  if ( mSymbolFeatureCounted ) return true;
  mSymbolFeatureCountMap.clear();

  if ( !mDataProvider )
  {
    QgsDebugMsg( "invoked with null mDataProvider" );
    return false;
  }
  if ( !mRendererV2 )
  {
    QgsDebugMsg( "invoked with null mRendererV2" );
    return false;
  }

  QgsLegendSymbolList symbolList = mRendererV2->legendSymbolItems();
  QgsLegendSymbolList::const_iterator symbolIt = symbolList.constBegin();

  for ( ; symbolIt != symbolList.constEnd(); ++symbolIt )
  {
    mSymbolFeatureCountMap.insert( symbolIt->second, 0 );
  }

  long nFeatures = pendingFeatureCount();
  QProgressDialog progressDialog( tr( "Updating feature count for layer %1" ).arg( name() ), tr( "Abort" ), 0, nFeatures );
  progressDialog.setWindowModality( Qt::WindowModal );
  int featuresCounted = 0;

  QgsFeatureIterator fit = getFeatures( QgsFeatureRequest().setFlags( QgsFeatureRequest::NoGeometry ) );

  // Renderer (rule based) may depend on context scale, with scale is ignored if 0
  QgsRenderContext renderContext;
  renderContext.setRendererScale( 0 );
  mRendererV2->startRender( renderContext, this );

  QgsFeature f;
  while ( fit.nextFeature( f ) )
  {
    QgsSymbolV2List featureSymbolList = mRendererV2->symbolsForFeature( f );
    for ( QgsSymbolV2List::iterator symbolIt = featureSymbolList.begin(); symbolIt != featureSymbolList.end(); ++symbolIt )
    {
      mSymbolFeatureCountMap[*symbolIt] += 1;
    }
    ++featuresCounted;

    if ( showProgress )
    {
      if ( featuresCounted % 50 == 0 )
      {
        if ( featuresCounted > nFeatures ) //sometimes the feature count is not correct
        {
          progressDialog.setMaximum( 0 );
        }
        progressDialog.setValue( featuresCounted );
        if ( progressDialog.wasCanceled() )
        {
          mSymbolFeatureCountMap.clear();
          mRendererV2->stopRender( renderContext );
          return false;
        }
      }
    }
  }
  mRendererV2->stopRender( renderContext );
  progressDialog.setValue( nFeatures );
  mSymbolFeatureCounted = true;
  return true;
}

void QgsVectorLayer::updateExtents()
{
  mValidExtent = false;
}

void QgsVectorLayer::setExtent( const QgsRectangle &r )
{
  QgsMapLayer::setExtent( r );
  mValidExtent = true;
}

QgsRectangle QgsVectorLayer::extent()
{
  if ( mValidExtent )
    return QgsMapLayer::extent();

  QgsRectangle rect;
  rect.setMinimal();

  if ( !hasGeometryType() )
    return rect;

  if ( !mDataProvider )
  {
    QgsDebugMsg( "invoked with null mDataProvider" );
  }

  if ( mEditBuffer && mEditBuffer->mDeletedFeatureIds.isEmpty() && mEditBuffer->mChangedGeometries.isEmpty() )
  {
    mDataProvider->updateExtents();

    // get the extent of the layer from the provider
    // but only when there are some features already
    if ( mDataProvider->featureCount() != 0 )
    {
      QgsRectangle r = mDataProvider->extent();
      rect.combineExtentWith( &r );
    }

    for ( QgsFeatureMap::iterator it = mEditBuffer->mAddedFeatures.begin(); it != mEditBuffer->mAddedFeatures.end(); it++ )
    {
      QgsRectangle r = it->geometry()->boundingBox();
      rect.combineExtentWith( &r );
    }
  }
  else
  {
    QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                          .setSubsetOfAttributes( QgsAttributeList() ) );

    QgsFeature fet;
    while ( fit.nextFeature( fet ) )
    {
      if ( fet.geometry() )
      {
        QgsRectangle bb = fet.geometry()->boundingBox();
        rect.combineExtentWith( &bb );
      }
    }
  }

  if ( rect.xMinimum() > rect.xMaximum() && rect.yMinimum() > rect.yMaximum() )
  {
    // special case when there are no features in provider nor any added
    rect = QgsRectangle(); // use rectangle with zero coordinates
  }

  setExtent( rect );

  // Send this (hopefully) up the chain to the map canvas
  emit recalculateExtents();

  return rect;
}

QString QgsVectorLayer::subsetString()
{
  if ( ! mDataProvider )
  {
    QgsDebugMsg( "invoked with null mDataProvider" );
    return 0;
  }
  return mDataProvider->subsetString();
}

bool QgsVectorLayer::setSubsetString( QString subset )
{
  if ( ! mDataProvider )
  {
    QgsDebugMsg( "invoked with null mDataProvider" );
    return false;
  }

  bool res = mDataProvider->setSubsetString( subset );

  // get the updated data source string from the provider
  mDataSource = mDataProvider->dataSourceUri();
  updateExtents();

  if ( res )
    setCacheImage( 0 );

  return res;
}

#if 0
void QgsVectorLayer::addJoinedAttributes( QgsFeature& f, bool all )
{
  if ( all || ( mFetchAttributes.size() > 0 && mJoinBuffer->containsFetchJoins() ) )
  {
    mJoinBuffer->updateFeatureAttributes( f, f.attributes().count(), all );
  }
}
#endif

QgsFeatureIterator QgsVectorLayer::getFeatures( const QgsFeatureRequest& request )
{
  if ( !mDataProvider )
    return QgsFeatureIterator();

  return QgsFeatureIterator( new QgsVectorLayerFeatureIterator( this, request ) );
}

void QgsVectorLayer::select( QgsAttributeList attributes, QgsRectangle rect, bool fetchGeometries, bool useIntersect )
{
  QgsFeatureRequest request;
  if ( !rect.isEmpty() )
    request.setFilterRect( rect );
  if ( !fetchGeometries )
    request.setFlags( QgsFeatureRequest::NoGeometry );
  if ( useIntersect )
    request.setFlags( request.flags() | QgsFeatureRequest::ExactIntersect );
  if ( attributes != pendingAllAttributesList() )
    request.setSubsetOfAttributes( attributes );

  if ( !mLayerIterator.isClosed() )
    mLayerIterator.close();

  mLayerIterator = getFeatures( request );
}

bool QgsVectorLayer::nextFeature( QgsFeature &f )
{
  return mLayerIterator.nextFeature( f );

#if 0
  if ( !mFetching )
    return false;

  if ( mEditBuffer )
  {
    if ( !mFetchRect.isEmpty() )
    {
      // check if changed geometries are in rectangle
      for ( ; mFetchChangedGeomIt != mEditBuffer->mChangedGeometries.end(); mFetchChangedGeomIt++ )
      {
        QgsFeatureId fid = mFetchChangedGeomIt.key();

        if ( mFetchConsidered.contains( fid ) )
          // skip deleted features
          continue;

        mFetchConsidered << fid;

        if ( !mFetchChangedGeomIt->intersects( mFetchRect ) )
          // skip changed geometries not in rectangle and don't check again
          continue;

        f.setFeatureId( fid );
        f.setValid( true );

        if ( mFetchGeometry )
          f.setGeometry( mFetchChangedGeomIt.value() );

        if ( mFetchAttributes.size() > 0 )
        {
          if ( fid < 0 )
          {
            // fid<0 => in mAddedFeatures
            bool found = false;

            for ( QgsFeatureList::iterator it = mEditBuffer->mAddedFeatures.begin(); it != mEditBuffer->mAddedFeatures.end(); it++ )
            {
              if ( fid == it->id() )
              {
                found = true;
                f.setAttributes( it->attributes() );
                // no need to update (always up-to-date) updateFeatureAttributes( f );
                break;
              }
            }

            if ( !found )
            {
              QgsDebugMsg( QString( "No attributes for the added feature %1 found" ).arg( f.id() ) );
            }
          }
          else
          {
            // retrieve attributes from provider
            QgsFeature tmp;
            //mDataProvider->featureAtId( fid, tmp, false, mFetchProvAttributes );
            QgsFeatureRequest request;
            request.setFilterFid( fid ).setSubsetOfAttributes( mFetchProvAttributes );
            QgsFeatureIterator fi = mDataProvider->getFeatures( request );
            if ( fi.nextFeature( tmp ) )
            {
              if ( mEditBuffer )
                mEditBuffer->updateChangedAttributes( tmp );
              f.setAttributes( tmp.attributes() );
            }
          }
        }

        addJoinedAttributes( f );

        // return complete feature
        mFetchChangedGeomIt++;
        return true;
      }

      // no more changed geometries
    }

    for ( ; mFetchAddedFeaturesIt != mEditBuffer->mAddedFeatures.end(); mFetchAddedFeaturesIt++ )
    {
      QgsFeatureId fid = mFetchAddedFeaturesIt->id();

      if ( mFetchConsidered.contains( fid ) )
        // must have changed geometry outside rectangle
        continue;

      if ( !mFetchRect.isEmpty() &&
           mFetchAddedFeaturesIt->geometry() &&
           !mFetchAddedFeaturesIt->geometry()->intersects( mFetchRect ) )
        // skip added features not in rectangle
        continue;

      f.setFeatureId( fid );
      f.setValid( true );

      if ( mFetchGeometry )
        f.setGeometry( *mFetchAddedFeaturesIt->geometry() );

      if ( mFetchAttributes.size() > 0 )
      {
        f.setAttributes( mFetchAddedFeaturesIt->attributes() );
        // no need to do this (always up-to-date) updateFeatureAttributes( f );
      }

      addJoinedAttributes( f );

      mFetchAddedFeaturesIt++;
      return true;
    }

    // no more added features
  }

  while ( mProviderIterator.nextFeature( f ) )
  {
    if ( mFetchConsidered.contains( f.id() ) )
    {
      continue;
    }
    if ( mFetchAttributes.size() > 0 )
    {
      if ( mEditBuffer )
        mEditBuffer->updateChangedAttributes( f ); //check changed attributes
      addJoinedAttributes( f ); // check joined attributes
    }
    if ( mEditBuffer && mFetchGeometry )
    {
      mEditBuffer->updateFeatureGeometry( f );
    }

    return true;
  }

  mFetching = false;
  return false;
#endif
}

bool QgsVectorLayer::featureAtId( QgsFeatureId featureId, QgsFeature& f, bool fetchGeometries, bool fetchAttributes )
{
  QgsFeatureRequest request;
  request.setFilterFid( featureId );
  if ( !fetchGeometries )
    request.setFlags( QgsFeatureRequest::NoGeometry );
  if ( !fetchAttributes )
    request.setSubsetOfAttributes( QgsAttributeList() );

  QgsFeatureIterator fi = getFeatures( request );
  return fi.nextFeature( f );

#if 0
  if ( !mDataProvider )
    return false;

  if ( mEditBuffer && mEditBuffer->mDeletedFeatureIds.contains( featureId ) )
    return false;

  if ( fetchGeometries && mEditBuffer->mChangedGeometries.contains( featureId ) )
  {
    f.setFeatureId( featureId );
    f.setValid( true );
    f.setGeometry( mEditBuffer->mChangedGeometries[featureId] );

    if ( fetchAttributes )
    {
      if ( featureId < 0 && mEditBuffer )
      {
        // featureId<0 => in mAddedFeatures
        bool found = false;

        for ( QgsFeatureList::iterator it = mEditBuffer->mAddedFeatures.begin(); it != mEditBuffer->mAddedFeatures.end(); it++ )
        {
          if ( featureId != it->id() )
          {
            found = true;
            f.setAttributes( it->attributes() );
            break;
          }
        }

        if ( !found )
        {
          QgsDebugMsg( QString( "No attributes for the added feature %1 found" ).arg( f.id() ) );
        }
      }
      else
      {
        // retrieve attributes from provider
        QgsFeature tmp;
        //mDataProvider->featureAtId( featureId, tmp, false, mDataProvider->attributeIndexes() );
        QgsFeatureRequest request;
        request.setFilterFid( featureId ).setFlags( QgsFeatureRequest::NoGeometry );
        QgsFeatureIterator fi = mDataProvider->getFeatures( request );
        if ( fi.nextFeature( tmp ) )
        {
          f.setAttributes( tmp.attributes() );
          if ( mEditBuffer )
            mEditBuffer->updateChangedAttributes( f );
        }
      }
      addJoinedAttributes( f, true );
    }
    return true;
  }

  //added features
  if ( mEditBuffer )
  {
    for ( QgsFeatureList::iterator iter = mEditBuffer->mAddedFeatures.begin(); iter != mEditBuffer->mAddedFeatures.end(); ++iter )
    {
      if ( iter->id() == featureId )
      {
        f.setFeatureId( iter->id() );
        f.setValid( true );
        if ( fetchGeometries )
          f.setGeometry( *iter->geometry() );

        if ( fetchAttributes )
          f.setAttributes( iter->attributes() );

        return true;
      }
    }
  }

  // regular features
  QgsFeatureRequest request;
  request.setFilterFid( featureId );
  if ( !fetchGeometries )
    request.setFlags( QgsFeatureRequest::NoGeometry );
  if ( !fetchAttributes )
    request.setSubsetOfAttributes( QgsAttributeList() );

  QgsFeatureIterator fi = mDataProvider->getFeatures( request );
  if ( fi.nextFeature( f ) )
  {
    if ( mEditBuffer )
      mEditBuffer->updateChangedAttributes( f );
    addJoinedAttributes( f, true );
    return true;
  }

  return false;
#endif
}

bool QgsVectorLayer::addFeature( QgsFeature& f, bool alsoUpdateExtent )
{
  Q_UNUSED( alsoUpdateExtent ); // TODO[MD]
  if ( !mEditBuffer || !mDataProvider )
    return false;

  return mEditBuffer->addFeature( f );
}

bool QgsVectorLayer::updateFeature( QgsFeature &f )
{
  QgsFeatureRequest req;
  req.setFilterFid( f.id() );
  if ( !f.geometry() )
    req.setFlags( QgsFeatureRequest::NoGeometry );
  if ( f.attributes().isEmpty() )
    req.setSubsetOfAttributes( QgsAttributeList() );

  QgsFeature current;
  if ( !getFeatures( req ).nextFeature( current ) )
  {
    QgsDebugMsg( QString( "feature %1 could not be retrieved" ).arg( f.id() ) );
    return false;
  }

  if ( f.geometry() && current.geometry() && f.geometry() != current.geometry() && !f.geometry()->isGeosEqual( *current.geometry() ) )
  {
    if ( !changeGeometry( f.id(), f.geometry() ) )
    {
      QgsDebugMsg( QString( "geometry of feature %1 could not be changed." ).arg( f.id() ) );
      return false;
    }
  }

  const QgsAttributes &fa = f.attributes();
  const QgsAttributes &ca = current.attributes();

  for ( int attr = 0; attr < fa.count(); ++attr )
  {
    if ( fa[attr] != ca[attr] )
    {
      if ( !changeAttributeValue( f.id(), attr, fa[attr] ) )
      {
        QgsDebugMsg( QString( "attribute %1 of feature %2 could not be changed." ).arg( attr ).arg( f.id() ) );
        return false;
      }
    }
  }

  return true;
}


bool QgsVectorLayer::insertVertex( double x, double y, QgsFeatureId atFeatureId, int beforeVertex )
{
  if ( !mEditBuffer || !mDataProvider )
    return false;

  QgsVectorLayerEditUtils utils( this );
  return utils.insertVertex( x, y, atFeatureId, beforeVertex );
}


bool QgsVectorLayer::moveVertex( double x, double y, QgsFeatureId atFeatureId, int atVertex )
{
  if ( !mEditBuffer || !mDataProvider )
    return false;

  QgsVectorLayerEditUtils utils( this );
  return utils.moveVertex( x, y, atFeatureId, atVertex );
}


bool QgsVectorLayer::deleteVertex( QgsFeatureId atFeatureId, int atVertex )
{
  if ( !mEditBuffer || !mDataProvider )
    return false;

  QgsVectorLayerEditUtils utils( this );
  return utils.deleteVertex( atFeatureId, atVertex );
}


bool QgsVectorLayer::deleteSelectedFeatures()
{
  if ( !( mDataProvider->capabilities() & QgsVectorDataProvider::DeleteFeatures ) )
  {
    return false;
  }

  if ( !isEditable() )
  {
    return false;
  }

  if ( mSelectedFeatureIds.size() == 0 )
    return true;

  while ( mSelectedFeatureIds.size() > 0 )
  {
    QgsFeatureId fid = *mSelectedFeatureIds.begin();
    deleteFeature( fid );  // removes from selection
  }

  // invalidate cache
  setCacheImage( 0 );

  emit selectionChanged();

  triggerRepaint();
  updateExtents();

  return true;
}

int QgsVectorLayer::addRing( const QList<QgsPoint>& ring )
{
  if ( !mEditBuffer || !mDataProvider )
    return 6;

  QgsVectorLayerEditUtils utils( this );
  return utils.addRing( ring );
}

int QgsVectorLayer::addPart( const QList<QgsPoint> &points )
{
  if ( !mEditBuffer || !mDataProvider )
    return 7;

  //number of selected features must be 1

  if ( mSelectedFeatureIds.size() < 1 )
  {
    QgsDebugMsg( "Number of selected features <1" );
    return 4;
  }
  else if ( mSelectedFeatureIds.size() > 1 )
  {
    QgsDebugMsg( "Number of selected features >1" );
    return 5;
  }

  QgsVectorLayerEditUtils utils( this );
  return utils.addPart( points, *mSelectedFeatureIds.constBegin() );
}


int QgsVectorLayer::translateFeature( QgsFeatureId featureId, double dx, double dy )
{
  if ( !mEditBuffer || !mDataProvider )
    return -1;

  QgsVectorLayerEditUtils utils( this );
  return utils.translateFeature( featureId, dx, dy );
}

int QgsVectorLayer::splitFeatures( const QList<QgsPoint>& splitLine, bool topologicalEditing )
{
  if ( !mEditBuffer || !mDataProvider )
    return -1;

  QgsVectorLayerEditUtils utils( this );
  return utils.splitFeatures( splitLine, topologicalEditing );
}

int QgsVectorLayer::removePolygonIntersections( QgsGeometry* geom, QgsFeatureIds ignoreFeatures )
{
  if ( !hasGeometryType() )
    return 1;

  int returnValue = 0;

  //first test if geom really has type polygon or multipolygon
  if ( geom->type() != QGis::Polygon )
  {
    return 1;
  }

  //get bounding box of geom
  QgsRectangle geomBBox = geom->boundingBox();

  //get list of features that intersect this bounding box
  QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                        .setFilterRect( geomBBox )
                                        .setFlags( QgsFeatureRequest::ExactIntersect )
                                        .setSubsetOfAttributes( QgsAttributeList() ) );

  QgsFeature f;
  while ( fit.nextFeature( f ) )
  {
    if ( ignoreFeatures.contains( f.id() ) )
    {
      continue;
    }

    //call geometry->makeDifference for each feature
    QgsGeometry *currentGeom = f.geometry();
    if ( currentGeom )
    {
      if ( geom->makeDifference( currentGeom ) != 0 )
      {
        returnValue = 2;
      }
    }
  }

  return returnValue;
}

int QgsVectorLayer::addTopologicalPoints( QgsGeometry* geom )
{
  if ( !mEditBuffer || !mDataProvider )
    return -1;

  QgsVectorLayerEditUtils utils( this );
  return utils.addTopologicalPoints( geom );
}

int QgsVectorLayer::addTopologicalPoints( const QgsPoint& p )
{
  if ( !mEditBuffer || !mDataProvider )
    return -1;

  QgsVectorLayerEditUtils utils( this );
  return utils.addTopologicalPoints( p );
}

QgsLabel *QgsVectorLayer::label()
{
  return mLabel;
}

const QgsLabel *QgsVectorLayer::label() const
{
  return mLabel;
}

void QgsVectorLayer::enableLabels( bool on )
{
  mLabelOn = on;
}

bool QgsVectorLayer::hasLabelsEnabled( void ) const
{
  return mLabelOn;
}

bool QgsVectorLayer::startEditing()
{
  if ( !mDataProvider )
  {
    return false;
  }

  // allow editing if provider supports any of the capabilities
  if ( !( mDataProvider->capabilities() & QgsVectorDataProvider::EditingCapabilities ) )
  {
    return false;
  }

  if ( mReadOnly )
  {
    return false;
  }

  if ( mEditBuffer )
  {
    // editing already underway
    return false;
  }

  mEditBuffer = new QgsVectorLayerEditBuffer( this );
  // forward signals
  connect( mEditBuffer, SIGNAL( layerModified() ), this, SIGNAL( layerModified() ) ); // TODO[MD]: necessary?
  //connect( mEditBuffer, SIGNAL( layerModified() ), this, SLOT( triggerRepaint() ) ); // TODO[MD]: works well?
  connect( mEditBuffer, SIGNAL( featureAdded( QgsFeatureId ) ), this, SIGNAL( featureAdded( QgsFeatureId ) ) );
  connect( mEditBuffer, SIGNAL( featureDeleted( QgsFeatureId ) ), this, SIGNAL( featureDeleted( QgsFeatureId ) ) );
  connect( mEditBuffer, SIGNAL( geometryChanged( QgsFeatureId, QgsGeometry& ) ), this, SIGNAL( geometryChanged( QgsFeatureId, QgsGeometry& ) ) );
  connect( mEditBuffer, SIGNAL( attributeValueChanged( QgsFeatureId, int, QVariant ) ), this, SIGNAL( attributeValueChanged( QgsFeatureId, int, QVariant ) ) );
  connect( mEditBuffer, SIGNAL( attributeAdded( int ) ), this, SIGNAL( attributeAdded( int ) ) );
  connect( mEditBuffer, SIGNAL( attributeDeleted( int ) ), this, SIGNAL( attributeDeleted( int ) ) );
  connect( mEditBuffer, SIGNAL( committedFeaturesAdded( QString, QgsFeatureList ) ), this, SIGNAL( committedFeaturesAdded( QString, QgsFeatureList ) ) );
  connect( mEditBuffer, SIGNAL( committedFeaturesRemoved( QString, QgsFeatureIds ) ), this, SIGNAL( committedFeaturesRemoved( QString, QgsFeatureIds ) ) );

  updateFields();

  emit editingStarted();

  return true;
}

bool QgsVectorLayer::readXml( const QDomNode& layer_node )
{
  QgsDebugMsg( QString( "Datasource in QgsVectorLayer::readXml: " ) + mDataSource.toLocal8Bit().data() );

  //process provider key
  QDomNode pkeyNode = layer_node.namedItem( "provider" );

  if ( pkeyNode.isNull() )
  {
    mProviderKey = "";
  }
  else
  {
    QDomElement pkeyElt = pkeyNode.toElement();
    mProviderKey = pkeyElt.text();
  }

  // determine type of vector layer
  if ( ! mProviderKey.isNull() )
  {
    // if the provider string isn't empty, then we successfully
    // got the stored provider
  }
  else if ( mDataSource.contains( "dbname=" ) )
  {
    mProviderKey = "postgres";
  }
  else
  {
    mProviderKey = "ogr";
  }

  if ( ! setDataProvider( mProviderKey ) )
  {
    return false;
  }

  QDomElement pkeyElem = pkeyNode.toElement();
  if ( !pkeyElem.isNull() )
  {
    QString encodingString = pkeyElem.attribute( "encoding" );
    if ( !encodingString.isEmpty() )
    {
      mDataProvider->setEncoding( encodingString );
    }
  }

  //load vector joins
  if ( !mJoinBuffer )
  {
    mJoinBuffer = new QgsVectorLayerJoinBuffer();
  }
  mJoinBuffer->readXml( layer_node );

  updateFields();
  connect( QgsMapLayerRegistry::instance(), SIGNAL( layerWillBeRemoved( QString ) ), this, SLOT( checkJoinLayerRemove( QString ) ) );

  QString errorMsg;
  if ( !readSymbology( layer_node, errorMsg ) )
  {
    return false;
  }

  return mValid;               // should be true if read successfully

} // void QgsVectorLayer::readXml



bool QgsVectorLayer::setDataProvider( QString const & provider )
{
  // XXX should I check for and possibly delete any pre-existing providers?
  // XXX How often will that scenario occur?

  mProviderKey = provider;     // XXX is this necessary?  Usually already set
  // XXX when execution gets here.

  //XXX - This was a dynamic cast but that kills the Windows
  //      version big-time with an abnormal termination error
  mDataProvider =
    ( QgsVectorDataProvider* )( QgsProviderRegistry::instance()->provider( provider, mDataSource ) );

  if ( mDataProvider )
  {
    QgsDebugMsg( "Instantiated the data provider plugin" );

    mValid = mDataProvider->isValid();
    if ( mValid )
    {

      // TODO: Check if the provider has the capability to send fullExtentCalculated
      connect( mDataProvider, SIGNAL( fullExtentCalculated() ), this, SLOT( updateExtents() ) );

      // get the extent
      QgsRectangle mbr = mDataProvider->extent();

      // show the extent
      QString s = mbr.toString();
      QgsDebugMsg( "Extent of layer: " +  s );
      // store the extent
      setExtent( mbr );

      // get and store the feature type
      mWkbType = mDataProvider->geometryType();

      // look at the fields in the layer and set the primary
      // display field using some real fuzzy logic
      setDisplayField();

      if ( mProviderKey == "postgres" )
      {
        QgsDebugMsg( "Beautifying layer name " + name() );

        // adjust the display name for postgres layers
        QRegExp reg( "\"[^\"]+\"\\.\"([^\"]+)\"( \\([^)]+\\))?" );
        if ( reg.indexIn( name() ) >= 0 )
        {
          QStringList stuff = reg.capturedTexts();
          QString lName = stuff[1];

          const QMap<QString, QgsMapLayer*> &layers = QgsMapLayerRegistry::instance()->mapLayers();

          QMap<QString, QgsMapLayer*>::const_iterator it;
          for ( it = layers.constBegin(); it != layers.constEnd() && ( *it )->name() != lName; it++ )
            ;

          if ( it != layers.constEnd() && stuff.size() > 2 )
          {
            lName += "." + stuff[2].mid( 2, stuff[2].length() - 3 );
          }

          if ( !lName.isEmpty() )
            setLayerName( lName );
        }

        QgsDebugMsg( "Beautified layer name " + name() );

        // deal with unnecessary schema qualification to make v.in.ogr happy
        mDataSource = mDataProvider->dataSourceUri();
      }
      else if ( mProviderKey == "osm" )
      {
        // make sure that the "observer" has been removed from URI to avoid crashes
        mDataSource = mDataProvider->dataSourceUri();
      }
      else if ( provider == "ogr" )
      {
        // make sure that the /vsigzip or /vsizip is added to uri, if applicable
        mDataSource = mDataProvider->dataSourceUri();
        if ( mDataSource.right( 10 ) == "|layerid=0" )
          mDataSource.chop( 10 );
      }

      // label
      mLabel = new QgsLabel( mDataProvider->fields() );
      mLabelOn = false;
    }
    else
    {
      QgsDebugMsg( "Invalid provider plugin " + QString( mDataSource.toUtf8() ) );
      return false;
    }
  }
  else
  {
    QgsDebugMsg( " unable to get data provider" );
    return false;
  }

  return true;

} // QgsVectorLayer:: setDataProvider




/* virtual */
bool QgsVectorLayer::writeXml( QDomNode & layer_node,
                               QDomDocument & document )
{
  // first get the layer element so that we can append the type attribute

  QDomElement mapLayerNode = layer_node.toElement();

  if ( mapLayerNode.isNull() || ( "maplayer" != mapLayerNode.nodeName() ) )
  {
    QgsDebugMsg( "can't find <maplayer>" );
    return false;
  }

  mapLayerNode.setAttribute( "type", "vector" );

  // set the geometry type
  mapLayerNode.setAttribute( "geometry", QGis::vectorGeometryType( geometryType() ) );

  // add provider node
  if ( mDataProvider )
  {
    QDomElement provider  = document.createElement( "provider" );
    provider.setAttribute( "encoding", mDataProvider->encoding() );
    QDomText providerText = document.createTextNode( providerType() );
    provider.appendChild( providerText );
    layer_node.appendChild( provider );
  }

  //save joins
  mJoinBuffer->writeXml( layer_node, document );

  // renderer specific settings
  QString errorMsg;
  return writeSymbology( layer_node, document, errorMsg );
} // bool QgsVectorLayer::writeXml

bool QgsVectorLayer::readSymbology( const QDomNode& node, QString& errorMessage )
{
  if ( hasGeometryType() )
  {
    // try renderer v2 first
    QDomElement rendererElement = node.firstChildElement( RENDERER_TAG_NAME );
    if ( !rendererElement.isNull() )
    {
      // using renderer v2
      setUsingRendererV2( true );

      QgsFeatureRendererV2* r = QgsFeatureRendererV2::load( rendererElement );
      if ( !r )
        return false;

      setRendererV2( r );
    }
    else
    {
      // using renderer v1
      setUsingRendererV2( false );

      // create and bind a renderer to this layer

      QDomNode singlenode = node.namedItem( "singlesymbol" );
      QDomNode graduatednode = node.namedItem( "graduatedsymbol" );
      QDomNode continuousnode = node.namedItem( "continuoussymbol" );
      QDomNode uniquevaluenode = node.namedItem( "uniquevalue" );

      QgsRenderer * renderer = 0;
      int returnCode = 1;

      if ( !singlenode.isNull() )
      {
        renderer = new QgsSingleSymbolRenderer( geometryType() );
        returnCode = renderer->readXML( singlenode, *this );
      }
      else if ( !graduatednode.isNull() )
      {
        renderer = new QgsGraduatedSymbolRenderer( geometryType() );
        returnCode = renderer->readXML( graduatednode, *this );
      }
      else if ( !continuousnode.isNull() )
      {
        renderer = new QgsContinuousColorRenderer( geometryType() );
        returnCode = renderer->readXML( continuousnode, *this );
      }
      else if ( !uniquevaluenode.isNull() )
      {
        renderer = new QgsUniqueValueRenderer( geometryType() );
        returnCode = renderer->readXML( uniquevaluenode, *this );
      }

      if ( !renderer )
      {
        errorMessage = tr( "Unknown renderer" );
        return false;
      }

      if ( returnCode == 1 )
      {
        errorMessage = tr( "No renderer object" );
        delete renderer;
        return false;
      }
      else if ( returnCode == 2 )
      {
        errorMessage = tr( "Classification field not found" );
        delete renderer;
        return false;
      }

      mRenderer = renderer;
    }

    // get and set the display field if it exists.
    QDomNode displayFieldNode = node.namedItem( "displayfield" );
    if ( !displayFieldNode.isNull() )
    {
      QDomElement e = displayFieldNode.toElement();
      setDisplayField( e.text() );
    }

    // use scale dependent visibility flag
    QDomElement e = node.toElement();
    mLabel->setScaleBasedVisibility( e.attribute( "scaleBasedLabelVisibilityFlag", "0" ) == "1" );
    mLabel->setMinScale( e.attribute( "minLabelScale", "1" ).toFloat() );
    mLabel->setMaxScale( e.attribute( "maxLabelScale", "100000000" ).toFloat() );

    //also restore custom properties (for labeling-ng)
    readCustomProperties( node, "labeling" );

    // Test if labeling is on or off
    QDomNode labelnode = node.namedItem( "label" );
    QDomElement element = labelnode.toElement();
    int hasLabelsEnabled = element.text().toInt();
    if ( hasLabelsEnabled < 1 )
    {
      enableLabels( false );
    }
    else
    {
      enableLabels( true );
    }

    QDomNode labelattributesnode = node.namedItem( "labelattributes" );

    if ( !labelattributesnode.isNull() )
    {
      QgsDebugMsg( "calling readXML" );
      mLabel->readXML( labelattributesnode );
    }

    //diagram renderer and diagram layer settings
    delete mDiagramRenderer; mDiagramRenderer = 0;
    QDomElement singleCatDiagramElem = node.firstChildElement( "SingleCategoryDiagramRenderer" );
    if ( !singleCatDiagramElem.isNull() )
    {
      mDiagramRenderer = new QgsSingleCategoryDiagramRenderer();
      mDiagramRenderer->readXML( singleCatDiagramElem );
    }
    QDomElement linearDiagramElem = node.firstChildElement( "LinearlyInterpolatedDiagramRenderer" );
    if ( !linearDiagramElem.isNull() )
    {
      mDiagramRenderer = new QgsLinearlyInterpolatedDiagramRenderer();
      mDiagramRenderer->readXML( linearDiagramElem );
    }

    if ( mDiagramRenderer )
    {
      QDomElement diagramSettingsElem = node.firstChildElement( "DiagramLayerSettings" );
      if ( !diagramSettingsElem.isNull() )
      {
        mDiagramLayerSettings = new QgsDiagramLayerSettings();
        mDiagramLayerSettings->readXML( diagramSettingsElem );
      }
    }
  }

  // process the attribute actions
  mActions->readXML( node );

  mEditTypes.clear();
  QDomNode editTypesNode = node.namedItem( "edittypes" );
  if ( !editTypesNode.isNull() )
  {
    QDomNodeList editTypeNodes = editTypesNode.childNodes();

    for ( int i = 0; i < editTypeNodes.size(); i++ )
    {
      QDomNode editTypeNode = editTypeNodes.at( i );
      QDomElement editTypeElement = editTypeNode.toElement();

      QString name = editTypeElement.attribute( "name" );
      if ( fieldNameIndex( name ) < -1 )
        continue;

      EditType editType = ( EditType ) editTypeElement.attribute( "type" ).toInt();
      mEditTypes.insert( name, editType );

      int editable = editTypeElement.attribute( "editable" , "1" ).toInt();
      mFieldEditables.insert( name, editable == 1);

      switch ( editType )
      {
        case ValueMap:
          if ( editTypeNode.hasChildNodes() )
          {
            mValueMaps.insert( name, QMap<QString, QVariant>() );

            QDomNodeList valueMapNodes = editTypeNode.childNodes();
            for ( int j = 0; j < valueMapNodes.size(); j++ )
            {
              QDomElement value = valueMapNodes.at( j ).toElement();
              mValueMaps[ name ].insert( value.attribute( "key" ), value.attribute( "value" ) );
            }
          }
          break;

        case EditRange:
        case SliderRange:
        case DialRange:
        {
          QVariant min = editTypeElement.attribute( "min" );
          QVariant max = editTypeElement.attribute( "max" );
          QVariant step = editTypeElement.attribute( "step" );

          mRanges[ name ] = RangeData( min, max, step );
        }
        break;

        case CheckBox:
          mCheckedStates[ name ] = QPair<QString, QString>( editTypeElement.attribute( "checked" ), editTypeElement.attribute( "unchecked" ) );
          break;

        case ValueRelation:
        {
          QString id = editTypeElement.attribute( "layer" );
          QString key = editTypeElement.attribute( "key" );
          QString value = editTypeElement.attribute( "value" );
          bool allowNull = editTypeElement.attribute( "allowNull" ) == "true";
          bool orderByValue = editTypeElement.attribute( "orderByValue" ) == "true";
          bool allowMulti = editTypeElement.attribute( "allowMulti", "false" ) == "true";

          QString filterExpression;
          if ( editTypeElement.hasAttribute( "filterAttributeColumn" ) &&
               editTypeElement.hasAttribute( "filterAttributeValue" ) )
          {
            filterExpression = QString( "\"%1\"='%2'" )
                               .arg( editTypeElement.attribute( "filterAttributeColumn" ) )
                               .arg( editTypeElement.attribute( "filterAttributeValue" ) );
          }
          else
          {
            filterExpression  = editTypeElement.attribute( "filterExpression", QString::null );
          }

          mValueRelations[ name ] = ValueRelationData( id, key, value, allowNull, orderByValue, allowMulti, filterExpression );
        }
        break;

        case Classification:
        case FileName:
        case Immutable:
        case Hidden:
        case LineEdit:
        case TextEdit:
        case Calendar:
        case Enumeration:
        case UniqueValues:
        case UniqueValuesEditable:
        case UuidGenerator:
          break;
      }
    }
  }

  QDomNode editFormNode = node.namedItem( "editform" );
  if ( !editFormNode.isNull() )
  {
    QDomElement e = editFormNode.toElement();
    mEditForm = QgsProject::instance()->readPath( e.text() );
  }

  QDomNode editFormInitNode = node.namedItem( "editforminit" );
  if ( !editFormInitNode.isNull() )
  {
    mEditFormInit = editFormInitNode.toElement().text();
  }

  QDomNode annotationFormNode = node.namedItem( "annotationform" );
  if ( !annotationFormNode.isNull() )
  {
    QDomElement e = annotationFormNode.toElement();
    mAnnotationForm = QgsProject::instance()->readPath( e.text() );
  }

  mAttributeAliasMap.clear();
  QDomNode aliasesNode = node.namedItem( "aliases" );
  if ( !aliasesNode.isNull() )
  {
    QDomElement aliasElem;
    QString name;

    QDomNodeList aliasNodeList = aliasesNode.toElement().elementsByTagName( "alias" );
    for ( int i = 0; i < aliasNodeList.size(); ++i )
    {
      aliasElem = aliasNodeList.at( i ).toElement();

      QString field;
      if ( aliasElem.hasAttribute( "field" ) )
      {
        field = aliasElem.attribute( "field" );
      }
      else
      {
        int index = aliasElem.attribute( "index" ).toInt();

        if ( index >= 0 && index < pendingFields().count() )
          field = pendingFields()[ index ].name();
      }

      mAttributeAliasMap.insert( field, aliasElem.attribute( "name" ) );
    }
  }

  // tab display
  QDomNode editorLayoutNode = node.namedItem( "editorlayout" );
  if ( editorLayoutNode.isNull() )
  {
    mEditorLayout = GeneratedLayout;
  }
  else
  {
    if ( editorLayoutNode.toElement().text() == "uifilelayout" )
    {
      mEditorLayout = UiFileLayout;
    }
    else if ( editorLayoutNode.toElement().text() == "tablayout" )
    {
      mEditorLayout = TabLayout;
    }
    else
    {
      mEditorLayout = GeneratedLayout;
    }
  }

  //Attributes excluded from WMS and WFS
  mExcludeAttributesWMS.clear();
  QDomNode excludeWMSNode = node.namedItem( "excludeAttributesWMS" );
  if ( !excludeWMSNode.isNull() )
  {
    QDomNodeList attributeNodeList = excludeWMSNode.toElement().elementsByTagName( "attribute" );
    for ( int i = 0; i < attributeNodeList.size(); ++i )
    {
      mExcludeAttributesWMS.insert( attributeNodeList.at( i ).toElement().text() );
    }
  }

  mExcludeAttributesWFS.clear();
  QDomNode excludeWFSNode = node.namedItem( "excludeAttributesWFS" );
  if ( !excludeWFSNode.isNull() )
  {
    QDomNodeList attributeNodeList = excludeWFSNode.toElement().elementsByTagName( "attribute" );
    for ( int i = 0; i < attributeNodeList.size(); ++i )
    {
      mExcludeAttributesWFS.insert( attributeNodeList.at( i ).toElement().text() );
    }
  }

  // tabs and groups display info
  mAttributeEditorElements.clear();
  QDomNode attributeEditorFormNode = node.namedItem( "attributeEditorForm" );
  QDomNodeList attributeEditorFormNodeList = attributeEditorFormNode.toElement().childNodes();

  for ( int i = 0; i < attributeEditorFormNodeList.size(); i++ )
  {
    QDomElement elem = attributeEditorFormNodeList.at( i ).toElement();

    QgsAttributeEditorElement *attributeEditorWidget = attributeEditorElementFromDomElement( elem, this );
    mAttributeEditorElements.append( attributeEditorWidget );
  }
  return true;
}

QgsAttributeEditorElement* QgsVectorLayer::attributeEditorElementFromDomElement( QDomElement &elem, QObject* parent )
{
  QgsAttributeEditorElement* newElement = NULL;

  if ( elem.tagName() == "attributeEditorContainer" )
  {
    QgsAttributeEditorContainer* container = new QgsAttributeEditorContainer( elem.attribute( "name" ), parent );

    QDomNodeList childNodeList = elem.childNodes();

    for ( int i = 0; i < childNodeList.size(); i++ )
    {
      QDomElement childElem = childNodeList.at( i ).toElement();
      QgsAttributeEditorElement* myElem = attributeEditorElementFromDomElement( childElem, container );
      container->addChildElement( myElem );
    }

    newElement = container;
  }
  else if ( elem.tagName() == "attributeEditorField" )
  {
    QString name = elem.attribute( "name" );
    int idx = *( dataProvider()->fieldNameMap() ).find( name );
    newElement = new QgsAttributeEditorField( name, idx, parent );
  }

  return newElement;
}

bool QgsVectorLayer::writeSymbology( QDomNode& node, QDomDocument& doc, QString& errorMessage ) const
{
  QDomElement mapLayerNode = node.toElement();

  if ( hasGeometryType() )
  {
    if ( mUsingRendererV2 )
    {
      QDomElement rendererElement = mRendererV2->save( doc );
      node.appendChild( rendererElement );
    }
    else
    {
      // use scale dependent visibility flag
      mapLayerNode.setAttribute( "scaleBasedLabelVisibilityFlag", mLabel->scaleBasedVisibility() ? 1 : 0 );
      mapLayerNode.setAttribute( "minLabelScale", QString::number( mLabel->minScale() ) );
      mapLayerNode.setAttribute( "maxLabelScale", QString::number( mLabel->maxScale() ) );

      //classification field(s)
      QgsAttributeList attributes = mRenderer->classificationAttributes();
      const QgsFields& providerFields = mDataProvider->fields();
      for ( QgsAttributeList::const_iterator it = attributes.begin(); it != attributes.end(); ++it )
      {
        QDomElement classificationElement = doc.createElement( "classificationattribute" );
        QDomText classificationText = doc.createTextNode( providerFields[*it].name() );
        classificationElement.appendChild( classificationText );
        node.appendChild( classificationElement );
      }

      // renderer settings
      const QgsRenderer * myRenderer = renderer();
      if ( myRenderer )
      {
        if ( !myRenderer->writeXML( node, doc, *this ) )
        {
          errorMessage = tr( "renderer failed to save" );
          return false;
        }
      }
      else
      {
        QgsDebugMsg( "no renderer" );
        errorMessage = tr( "no renderer" );
        return false;
      }
    }

    //save customproperties (for labeling ng)
    writeCustomProperties( node, doc );

    // add the display field
    QDomElement dField  = doc.createElement( "displayfield" );
    QDomText dFieldText = doc.createTextNode( displayField() );
    dField.appendChild( dFieldText );
    node.appendChild( dField );

    // add label node
    QDomElement labelElem = doc.createElement( "label" );
    QDomText labelText = doc.createTextNode( "" );

    if ( hasLabelsEnabled() )
    {
      labelText.setData( "1" );
    }
    else
    {
      labelText.setData( "0" );
    }
    labelElem.appendChild( labelText );

    node.appendChild( labelElem );

    // Now we get to do all that all over again for QgsLabel

    QString fieldname = mLabel->labelField( QgsLabel::Text );
    if ( fieldname != "" )
    {
      dField  = doc.createElement( "labelfield" );
      dFieldText = doc.createTextNode( fieldname );
      dField.appendChild( dFieldText );
      node.appendChild( dField );
    }

    mLabel->writeXML( node, doc );

    if ( mDiagramRenderer )
    {
      mDiagramRenderer->writeXML( mapLayerNode, doc );
      if ( mDiagramLayerSettings )
        mDiagramLayerSettings->writeXML( mapLayerNode, doc );
    }
  }

  //edit types
  if ( mEditTypes.size() > 0 )
  {
    QDomElement editTypesElement = doc.createElement( "edittypes" );

    for ( QMap<QString, EditType>::const_iterator it = mEditTypes.begin(); it != mEditTypes.end(); ++it )
    {
      QDomElement editTypeElement = doc.createElement( "edittype" );
      editTypeElement.setAttribute( "name", it.key() );
      editTypeElement.setAttribute( "type", it.value() );
      editTypeElement.setAttribute( "editable", mFieldEditables[ it.key()]?1:0 );

      switch (( EditType ) it.value() )
      {
        case ValueMap:
          if ( mValueMaps.contains( it.key() ) )
          {
            const QMap<QString, QVariant> &map = mValueMaps[ it.key()];

            for ( QMap<QString, QVariant>::const_iterator vmit = map.begin(); vmit != map.end(); vmit++ )
            {
              QDomElement value = doc.createElement( "valuepair" );
              value.setAttribute( "key", vmit.key() );
              value.setAttribute( "value", vmit.value().toString() );
              editTypeElement.appendChild( value );
            }
          }
          break;

        case EditRange:
        case SliderRange:
        case DialRange:
          if ( mRanges.contains( it.key() ) )
          {
            editTypeElement.setAttribute( "min", mRanges[ it.key()].mMin.toString() );
            editTypeElement.setAttribute( "max", mRanges[ it.key()].mMax.toString() );
            editTypeElement.setAttribute( "step", mRanges[ it.key()].mStep.toString() );
          }
          break;

        case CheckBox:
          if ( mCheckedStates.contains( it.key() ) )
          {
            editTypeElement.setAttribute( "checked", mCheckedStates[ it.key()].first );
            editTypeElement.setAttribute( "unchecked", mCheckedStates[ it.key()].second );
          }
          break;

        case ValueRelation:
          if ( mValueRelations.contains( it.key() ) )
          {
            const ValueRelationData &data = mValueRelations[ it.key()];
            editTypeElement.setAttribute( "layer", data.mLayer );
            editTypeElement.setAttribute( "key", data.mKey );
            editTypeElement.setAttribute( "value", data.mValue );
            editTypeElement.setAttribute( "allowNull", data.mAllowNull ? "true" : "false" );
            editTypeElement.setAttribute( "orderByValue", data.mOrderByValue ? "true" : "false" );
            editTypeElement.setAttribute( "allowMulti", data.mAllowMulti ? "true" : "false" );
            if ( !data.mFilterExpression.isNull() )
              editTypeElement.setAttribute( "filterExpression", data.mFilterExpression );
          }
          break;

        case LineEdit:
        case UniqueValues:
        case UniqueValuesEditable:
        case Classification:
        case FileName:
        case Hidden:
        case TextEdit:
        case Calendar:
        case Enumeration:
        case Immutable:
        case UuidGenerator:
          break;
      }

      editTypesElement.appendChild( editTypeElement );
    }

    node.appendChild( editTypesElement );
  }

  QDomElement efField  = doc.createElement( "editform" );
  QDomText efText = doc.createTextNode( QgsProject::instance()->writePath( mEditForm ) );
  efField.appendChild( efText );
  node.appendChild( efField );

  QDomElement efiField  = doc.createElement( "editforminit" );
  QDomText efiText = doc.createTextNode( mEditFormInit );
  efiField.appendChild( efiText );
  node.appendChild( efiField );

  QDomElement afField = doc.createElement( "annotationform" );
  QDomText afText = doc.createTextNode( QgsProject::instance()->writePath( mAnnotationForm ) );
  afField.appendChild( afText );
  node.appendChild( afField );

  // tab display
  QDomElement editorLayoutElem  = doc.createElement( "editorlayout" );
  switch ( mEditorLayout )
  {
    case UiFileLayout:
      editorLayoutElem.appendChild( doc.createTextNode( "uifilelayout" ) );
      break;

    case TabLayout:
      editorLayoutElem.appendChild( doc.createTextNode( "tablayout" ) );
      break;

    case GeneratedLayout:
    default:
      editorLayoutElem.appendChild( doc.createTextNode( "generatedlayout" ) );
      break;
  }

  node.appendChild( editorLayoutElem );

  //attribute aliases
  if ( mAttributeAliasMap.size() > 0 )
  {
    QDomElement aliasElem = doc.createElement( "aliases" );
    QMap<QString, QString>::const_iterator a_it = mAttributeAliasMap.constBegin();
    for ( ; a_it != mAttributeAliasMap.constEnd(); ++a_it )
    {
      int idx = fieldNameIndex( a_it.key() );
      if ( idx < 0 )
        continue;

      QDomElement aliasEntryElem = doc.createElement( "alias" );
      aliasEntryElem.setAttribute( "field", a_it.key() );
      aliasEntryElem.setAttribute( "index", idx );
      aliasEntryElem.setAttribute( "name", a_it.value() );
      aliasElem.appendChild( aliasEntryElem );
    }
    node.appendChild( aliasElem );
  }

  //exclude attributes WMS
  QDomElement excludeWMSElem = doc.createElement( "excludeAttributesWMS" );
  QSet<QString>::const_iterator attWMSIt = mExcludeAttributesWMS.constBegin();
  for ( ; attWMSIt != mExcludeAttributesWMS.constEnd(); ++attWMSIt )
  {
    QDomElement attrElem = doc.createElement( "attribute" );
    QDomText attrText = doc.createTextNode( *attWMSIt );
    attrElem.appendChild( attrText );
    excludeWMSElem.appendChild( attrElem );
  }
  node.appendChild( excludeWMSElem );

  //exclude attributes WFS
  QDomElement excludeWFSElem = doc.createElement( "excludeAttributesWFS" );
  QSet<QString>::const_iterator attWFSIt = mExcludeAttributesWFS.constBegin();
  for ( ; attWFSIt != mExcludeAttributesWFS.constEnd(); ++attWFSIt )
  {
    QDomElement attrElem = doc.createElement( "attribute" );
    QDomText attrText = doc.createTextNode( *attWFSIt );
    attrElem.appendChild( attrText );
    excludeWFSElem.appendChild( attrElem );
  }
  node.appendChild( excludeWFSElem );

  // tabs and groups of edit form
  if ( mAttributeEditorElements.size() > 0 )
  {
    QDomElement tabsElem = doc.createElement( "attributeEditorForm" );

    for ( QList< QgsAttributeEditorElement* >::const_iterator it = mAttributeEditorElements.begin(); it != mAttributeEditorElements.end(); it++ )
    {
      QDomElement attributeEditorWidgetElem = ( *it )->toDomElement( doc );
      tabsElem.appendChild( attributeEditorWidgetElem );
    }

    node.appendChild( tabsElem );
  }

  // add attribute actions
  mActions->writeXML( node, doc );

  //save vector overlays (e.g. diagrams)
  QList<QgsVectorOverlay*>::const_iterator overlay_it = mOverlays.constBegin();
  for ( ; overlay_it != mOverlays.constEnd(); ++overlay_it )
  {
    if ( *overlay_it )
    {
      ( *overlay_it )->writeXML( mapLayerNode, doc );
    }
  }

  return true;
}

bool QgsVectorLayer::readSld( const QDomNode& node, QString& errorMessage )
{
  // get the Name element
  QDomElement nameElem = node.firstChildElement( "Name" );
  if ( nameElem.isNull() )
  {
    errorMessage = "Warning: Name element not found within NamedLayer while it's required.";
  }

  if ( hasGeometryType() )
  {
    setUsingRendererV2( true );

    QgsFeatureRendererV2* r = QgsFeatureRendererV2::loadSld( node, geometryType(), errorMessage );
    if ( !r )
      return false;

    setRendererV2( r );
  }
  return true;
}


bool QgsVectorLayer::writeSld( QDomNode& node, QDomDocument& doc, QString& errorMessage ) const
{
  Q_UNUSED( errorMessage );

  // store the Name element
  QDomElement nameNode = doc.createElement( "se:Name" );
  nameNode.appendChild( doc.createTextNode( name() ) );
  node.appendChild( nameNode );

  if ( hasGeometryType() )
  {
    if ( mUsingRendererV2 )
    {
      node.appendChild( mRendererV2->writeSld( doc, *this ) );
    }
    else
    {
      node.appendChild( doc.createComment( "Old Renderer not supported yet" ) );
      return false;
    }
  }
  return true;
}


bool QgsVectorLayer::changeGeometry( QgsFeatureId fid, QgsGeometry* geom )
{
  if ( !mEditBuffer || !mDataProvider )
  {
    return false;
  }

  return mEditBuffer->changeGeometry( fid, geom );
}


bool QgsVectorLayer::changeAttributeValue( QgsFeatureId fid, int field, QVariant value, bool emitSignal )
{
  Q_UNUSED( emitSignal ); // TODO[MD] - see also QgsFieldCalculator and #7071
  if ( !mEditBuffer || !mDataProvider )
    return false;

  return mEditBuffer->changeAttributeValue( fid, field, value );
}

bool QgsVectorLayer::addAttribute( const QgsField &field )
{
  if ( !mEditBuffer || !mDataProvider )
    return false;

  return mEditBuffer->addAttribute( field );
}

void QgsVectorLayer::addAttributeAlias( int attIndex, QString aliasString )
{
  if ( attIndex < 0 || attIndex >= pendingFields().count() )
    return;

  QString name = pendingFields()[ attIndex ].name();

  mAttributeAliasMap.insert( name, aliasString );
  emit layerModified(); // TODO[MD]: should have a different signal?
}

void QgsVectorLayer::addAttributeEditorWidget( QgsAttributeEditorElement* data )
{
  mAttributeEditorElements.append( data );
}

QString QgsVectorLayer::attributeAlias( int attributeIndex ) const
{
  if ( attributeIndex < 0 || attributeIndex >= pendingFields().count() )
    return "";

  QString name = pendingFields()[ attributeIndex ].name();

  return mAttributeAliasMap.value( name, "" );
}

QString QgsVectorLayer::attributeDisplayName( int attributeIndex ) const
{
  QString displayName = attributeAlias( attributeIndex );
  if ( displayName.isEmpty() )
  {
    const QgsFields& fields = pendingFields();
    if ( attributeIndex >= 0 && attributeIndex < fields.count() )
    {
      displayName = fields[attributeIndex].name();
    }
  }
  return displayName;
}

bool QgsVectorLayer::deleteAttribute( int index )
{
  if ( !mEditBuffer || !mDataProvider )
    return false;

  return mEditBuffer->deleteAttribute( index );
}

bool QgsVectorLayer::deleteFeature( QgsFeatureId fid )
{
  if ( !mEditBuffer )
    return false;

  bool res = mEditBuffer->deleteFeature( fid );
  if ( res )
    mSelectedFeatureIds.remove( fid ); // remove it from selection

  return res;
}

const QgsFields &QgsVectorLayer::pendingFields() const
{
  return mUpdatedFields;
}

QgsAttributeList QgsVectorLayer::pendingAllAttributesList()
{
  QgsAttributeList lst;
  for ( int i = 0; i < mUpdatedFields.count(); ++i )
    lst.append( i );
  return lst;
}

QgsAttributeList QgsVectorLayer::pendingPkAttributesList()
{
  QgsAttributeList pkAttributesList;

  QgsAttributeList providerIndexes = mDataProvider->pkAttributeIndexes();
  for ( int i = 0; i < mUpdatedFields.count(); ++i )
  {
    if ( mUpdatedFields.fieldOrigin( i ) == QgsFields::OriginProvider &&
         providerIndexes.contains( mUpdatedFields.fieldOriginIndex( i ) ) )
      pkAttributesList << i;
  }

  return pkAttributesList;
}

int QgsVectorLayer::pendingFeatureCount()
{
  return mDataProvider->featureCount() +
         ( mEditBuffer ? mEditBuffer->mAddedFeatures.size() - mEditBuffer->mDeletedFeatureIds.size() : 0 );
}

bool QgsVectorLayer::commitChanges()
{
  mCommitErrors.clear();

  if ( !mDataProvider )
  {
    mCommitErrors << tr( "ERROR: no provider" );
    return false;
  }

  if ( !mEditBuffer )
  {
    mCommitErrors << tr( "ERROR: layer not editable" );
    return false;
  }

  bool success = mEditBuffer->commitChanges( mCommitErrors );

  if ( success )
  {
    delete mEditBuffer;
    mEditBuffer = 0;
    undoStack()->clear();
    emit editingStopped();
  }
  else
  {
    QgsMessageLog::logMessage( tr( "Commit errors:\n  %1" ).arg( mCommitErrors.join( "\n  " ) ) );
  }

  updateFields();
  mDataProvider->updateExtents();

  //clear the cache image so markers don't appear anymore on next draw
  setCacheImage( 0 );

  return success;
}

const QStringList &QgsVectorLayer::commitErrors()
{
  return mCommitErrors;
}

bool QgsVectorLayer::rollBack( bool deleteBuffer )
{
  if ( !mEditBuffer )
  {
    return false;
  }

  mEditBuffer->rollBack();

  if ( isModified() )
  {
    // new undo stack roll back method
    // old method of calling every undo could cause many canvas refreshes
    undoStack()->setIndex( 0 );
  }

  updateFields();

  if ( deleteBuffer )
  {
    delete mEditBuffer;
    mEditBuffer = 0;
    undoStack()->clear();
  }
  emit editingStopped();

  // invalidate the cache so the layer updates properly to show its original
  // after the rollback
  setCacheImage( 0 );
  return true;
}

void QgsVectorLayer::setSelectedFeatures( const QgsFeatureIds& ids )
{
  // TODO: check whether features with these ID exist
  mSelectedFeatureIds = ids;

  // invalidate cache
  setCacheImage( 0 );

  emit selectionChanged();
}

int QgsVectorLayer::selectedFeatureCount()
{
  return mSelectedFeatureIds.size();
}

const QgsFeatureIds& QgsVectorLayer::selectedFeaturesIds() const
{
  return mSelectedFeatureIds;
}


QgsFeatureList QgsVectorLayer::selectedFeatures()
{
  QgsFeatureList features;

  QgsFeatureRequest req;
  if ( geometryType() == QGis::NoGeometry )
    req.setFlags( QgsFeatureRequest::NoGeometry );

  foreach ( QgsFeatureId fid, mSelectedFeatureIds )
  {
    features.push_back( QgsFeature() );
    getFeatures( req.setFilterFid( fid ) ).nextFeature( features.back() );
  }

  return features;
}

bool QgsVectorLayer::addFeatures( QgsFeatureList features, bool makeSelected )
{
  if ( !mEditBuffer || !mDataProvider )
    return false;

  bool res = mEditBuffer->addFeatures( features );

  if ( makeSelected )
  {
    mSelectedFeatureIds.clear();
    for ( QgsFeatureList::iterator iter = features.begin(); iter != features.end(); ++iter )
      mSelectedFeatureIds.insert( iter->id() );

    // invalidate cache
    setCacheImage( 0 );

    emit selectionChanged();
  }

  return res;
}


bool QgsVectorLayer::snapPoint( QgsPoint& point, double tolerance )
{
  if ( !hasGeometryType() )
    return false;

  QMultiMap<double, QgsSnappingResult> snapResults;
  int result = snapWithContext( point, tolerance, snapResults, QgsSnapper::SnapToVertex );

  if ( result != 0 )
  {
    return false;
  }

  if ( snapResults.size() < 1 )
  {
    return false;
  }

  QMultiMap<double, QgsSnappingResult>::const_iterator snap_it = snapResults.constBegin();
  point.setX( snap_it.value().snappedVertex.x() );
  point.setY( snap_it.value().snappedVertex.y() );
  return true;
}


int QgsVectorLayer::snapWithContext( const QgsPoint& startPoint, double snappingTolerance,
                                     QMultiMap<double, QgsSnappingResult>& snappingResults,
                                     QgsSnapper::SnappingType snap_to )
{
  if ( !hasGeometryType() )
    return 1;

  if ( snappingTolerance <= 0 || !mDataProvider )
  {
    return 1;
  }

  QList<QgsFeature> featureList;
  QgsRectangle searchRect( startPoint.x() - snappingTolerance, startPoint.y() - snappingTolerance,
                           startPoint.x() + snappingTolerance, startPoint.y() + snappingTolerance );
  double sqrSnappingTolerance = snappingTolerance * snappingTolerance;

  int n = 0;
  QgsFeature f;

  if ( mCache->cachedGeometriesRect().contains( searchRect ) )
  {
    QgsGeometryMap& cachedGeometries = mCache->cachedGeometries();
    for ( QgsGeometryMap::iterator it = cachedGeometries.begin(); it != cachedGeometries.end() ; ++it )
    {
      QgsGeometry* g = &( it.value() );
      if ( g->boundingBox().intersects( searchRect ) )
      {
        snapToGeometry( startPoint, it.key(), g, sqrSnappingTolerance, snappingResults, snap_to );
        ++n;
      }
    }
  }
  else
  {
    // snapping outside cached area

    QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                          .setFilterRect( searchRect )
                                          .setFlags( QgsFeatureRequest::ExactIntersect )
                                          .setSubsetOfAttributes( QgsAttributeList() ) );

    while ( fit.nextFeature( f ) )
    {
      snapToGeometry( startPoint, f.id(), f.geometry(), sqrSnappingTolerance, snappingResults, snap_to );
      ++n;
    }
  }

  return n == 0 ? 2 : 0;
}

void QgsVectorLayer::snapToGeometry( const QgsPoint& startPoint,
                                     QgsFeatureId featureId,
                                     QgsGeometry* geom,
                                     double sqrSnappingTolerance,
                                     QMultiMap<double, QgsSnappingResult>& snappingResults,
                                     QgsSnapper::SnappingType snap_to ) const
{
  if ( !geom )
  {
    return;
  }

  int atVertex, beforeVertex, afterVertex;
  double sqrDistVertexSnap, sqrDistSegmentSnap;
  QgsPoint snappedPoint;
  QgsSnappingResult snappingResultVertex;
  QgsSnappingResult snappingResultSegment;

  if ( snap_to == QgsSnapper::SnapToVertex || snap_to == QgsSnapper::SnapToVertexAndSegment )
  {
    snappedPoint = geom->closestVertex( startPoint, atVertex, beforeVertex, afterVertex, sqrDistVertexSnap );
    if ( sqrDistVertexSnap < sqrSnappingTolerance )
    {
      snappingResultVertex.snappedVertex = snappedPoint;
      snappingResultVertex.snappedVertexNr = atVertex;
      snappingResultVertex.beforeVertexNr = beforeVertex;
      if ( beforeVertex != -1 ) // make sure the vertex is valid
      {
        snappingResultVertex.beforeVertex = geom->vertexAt( beforeVertex );
      }
      snappingResultVertex.afterVertexNr = afterVertex;
      if ( afterVertex != -1 ) // make sure the vertex is valid
      {
        snappingResultVertex.afterVertex = geom->vertexAt( afterVertex );
      }
      snappingResultVertex.snappedAtGeometry = featureId;
      snappingResultVertex.layer = this;
      snappingResults.insert( sqrt( sqrDistVertexSnap ), snappingResultVertex );
      return;
    }
  }
  if ( snap_to == QgsSnapper::SnapToSegment || snap_to == QgsSnapper::SnapToVertexAndSegment ) // snap to segment
  {
    if ( geometryType() != QGis::Point ) // cannot snap to segment for points/multipoints
    {
      sqrDistSegmentSnap = geom->closestSegmentWithContext( startPoint, snappedPoint, afterVertex, NULL, crs().geographicFlag() ? 1e-12 : 1e-8 );

      if ( sqrDistSegmentSnap < sqrSnappingTolerance )
      {
        snappingResultSegment.snappedVertex = snappedPoint;
        snappingResultSegment.snappedVertexNr = -1;
        snappingResultSegment.beforeVertexNr = afterVertex - 1;
        snappingResultSegment.afterVertexNr = afterVertex;
        snappingResultSegment.snappedAtGeometry = featureId;
        snappingResultSegment.beforeVertex = geom->vertexAt( afterVertex - 1 );
        snappingResultSegment.afterVertex = geom->vertexAt( afterVertex );
        snappingResultSegment.layer = this;
        snappingResults.insert( sqrt( sqrDistSegmentSnap ), snappingResultSegment );
      }
    }
  }
}

int QgsVectorLayer::insertSegmentVerticesForSnap( const QList<QgsSnappingResult>& snapResults )
{
  QgsVectorLayerEditUtils utils( this );
  return utils.insertSegmentVerticesForSnap( snapResults );
}


QgsVectorLayer::VertexMarkerType QgsVectorLayer::currentVertexMarkerType()
{
  QSettings settings;
  QString markerTypeString = settings.value( "/qgis/digitizing/marker_style", "Cross" ).toString();
  if ( markerTypeString == "Cross" )
  {
    return QgsVectorLayer::Cross;
  }
  else if ( markerTypeString == "SemiTransparentCircle" )
  {
    return QgsVectorLayer::SemiTransparentCircle;
  }
  else
  {
    return QgsVectorLayer::NoMarker;
  }
}

int QgsVectorLayer::currentVertexMarkerSize()
{
  QSettings settings;
  return settings.value( "/qgis/digitizing/marker_size", 3 ).toInt();
}

void QgsVectorLayer::drawFeature( QgsRenderContext &renderContext,
                                  QgsFeature& fet,
                                  QImage * marker )
{
  QPainter *p = renderContext.painter();
  // Only have variables, etc outside the switch() statement that are
  // used in all cases of the statement (otherwise they may get
  // executed, but never used, in a bit of code where performance is
  // critical).
  if ( ! fet.isValid() )
  {
    return;
  }

  QgsGeometry* geom = fet.geometry();
  if ( !geom )
  {
    return;
  }
  unsigned char* feature = geom->asWkb();

  QGis::WkbType wkbType = geom->wkbType();

  switch ( wkbType )
  {
    case QGis::WKBPoint:
    case QGis::WKBPoint25D:
    {
      double x = *(( double * )( feature + 5 ) );
      double y = *(( double * )( feature + 5 + sizeof( double ) ) );

      transformPoint( x, y, &renderContext.mapToPixel(), renderContext.coordinateTransform() );
      if ( qAbs( x ) > QgsClipper::MAX_X ||
           qAbs( y ) > QgsClipper::MAX_Y )
      {
        break;
      }

      //QPointF pt(x - (marker->width()/2),  y - (marker->height()/2));
      QPointF pt( x * renderContext.rasterScaleFactor() - ( marker->width() / 2 ),
                  y * renderContext.rasterScaleFactor() - ( marker->height() / 2 ) );

      p->save();
      //p->scale(markerScaleFactor,markerScaleFactor);
      p->scale( 1.0 / renderContext.rasterScaleFactor(), 1.0 / renderContext.rasterScaleFactor() );
      p->drawImage( pt, *marker );
      p->restore();

      break;
    }
    case QGis::WKBMultiPoint:
    case QGis::WKBMultiPoint25D:
    {
      unsigned char *ptr = feature + 5;
      unsigned int nPoints = *(( int* )ptr );
      ptr += 4;

      p->save();
      //p->scale(markerScaleFactor, markerScaleFactor);
      p->scale( 1.0 / renderContext.rasterScaleFactor(), 1.0 / renderContext.rasterScaleFactor() );

      for ( register unsigned int i = 0; i < nPoints; ++i )
      {
        ptr += 5;
        double x = *(( double * ) ptr );
        ptr += sizeof( double );
        double y = *(( double * ) ptr );
        ptr += sizeof( double );

        if ( wkbType == QGis::WKBMultiPoint25D ) // ignore Z value
          ptr += sizeof( double );

        transformPoint( x, y, &renderContext.mapToPixel(), renderContext.coordinateTransform() );
        //QPointF pt(x - (marker->width()/2),  y - (marker->height()/2));
        //QPointF pt(x/markerScaleFactor - (marker->width()/2),  y/markerScaleFactor - (marker->height()/2));
        QPointF pt( x * renderContext.rasterScaleFactor() - ( marker->width() / 2 ),
                    y * renderContext.rasterScaleFactor() - ( marker->height() / 2 ) );
        //QPointF pt( x, y );

        // Work around a +/- 32768 limitation on coordinates
        if ( qAbs( x ) <= QgsClipper::MAX_X &&
             qAbs( y ) <= QgsClipper::MAX_Y )
          p->drawImage( pt, *marker );
      }
      p->restore();

      break;
    }
    case QGis::WKBLineString:
    case QGis::WKBLineString25D:
    {
      drawLineString( feature, renderContext );
      break;
    }
    case QGis::WKBMultiLineString:
    case QGis::WKBMultiLineString25D:
    {
      unsigned char* ptr = feature + 5;
      unsigned int numLineStrings = *(( int* )ptr );
      ptr = feature + 9;

      for ( register unsigned int jdx = 0; jdx < numLineStrings; jdx++ )
      {
        ptr = drawLineString( ptr, renderContext );
      }
      break;
    }
    case QGis::WKBPolygon:
    case QGis::WKBPolygon25D:
    {
      drawPolygon( feature, renderContext );
      break;
    }
    case QGis::WKBMultiPolygon:
    case QGis::WKBMultiPolygon25D:
    {
      unsigned char *ptr = feature + 5;
      unsigned int numPolygons = *(( int* )ptr );
      ptr = feature + 9;
      for ( register unsigned int kdx = 0; kdx < numPolygons; kdx++ )
        ptr = drawPolygon( ptr, renderContext );
      break;
    }
    default:
      QgsDebugMsg( QString( "Unknown WkbType 0x%1 ENCOUNTERED" ).arg( wkbType, 0, 16 ) );
      break;
  }
}



void QgsVectorLayer::setCoordinateSystem()
{
  QgsDebugMsg( "----- Computing Coordinate System" );

  //
  // Get the layers project info and set up the QgsCoordinateTransform
  // for this layer
  //

  if ( hasGeometryType() )
  {
    // get CRS directly from provider
    setCrs( mDataProvider->crs() );
  }
  else
  {
    setCrs( QgsCoordinateReferenceSystem( GEO_EPSG_CRS_AUTHID ) );
  }
}

// Convenience function to transform the given point
inline void QgsVectorLayer::transformPoint(
  double& x, double& y,
  const QgsMapToPixel* mtp,
  const QgsCoordinateTransform* ct )
{
  // transform the point
  if ( ct )
  {
    double z = 0;
    ct->transformInPlace( x, y, z );
  }

  // transform from projected coordinate system to pixel
  // position on map canvas
  mtp->transformInPlace( x, y );
}

inline void QgsVectorLayer::transformPoints(
  QVector<double>& x, QVector<double>& y, QVector<double>& z,
  QgsRenderContext &renderContext )
{
  // transform the point
  if ( renderContext.coordinateTransform() )
    renderContext.coordinateTransform()->transformInPlace( x, y, z );

  // transform from projected coordinate system to pixel
  // position on map canvas
  renderContext.mapToPixel().transformInPlace( x, y );
}


const QString QgsVectorLayer::displayField() const
{
  return mDisplayField;
}

bool QgsVectorLayer::isEditable() const
{
  return ( mEditBuffer && mDataProvider );
}

bool QgsVectorLayer::isReadOnly() const
{
  return mReadOnly;
}

bool QgsVectorLayer::setReadOnly( bool readonly )
{
  // exit if the layer is in editing mode
  if ( readonly && mEditBuffer )
    return false;

  mReadOnly = readonly;
  return true;
}

bool QgsVectorLayer::isModified() const
{
  return mEditBuffer && mEditBuffer->isModified();
}

QgsVectorLayer::EditType QgsVectorLayer::editType( int idx )
{
  const QgsFields &fields = pendingFields();
  if ( idx >= 0 && idx < fields.count() && mEditTypes.contains( fields[idx].name() ) )
    return mEditTypes[ fields[idx].name()];
  else
    return LineEdit;
}

void QgsVectorLayer::setEditType( int idx, EditType type )
{
  const QgsFields &fields = pendingFields();
  if ( idx >= 0 && idx < fields.count() )
    mEditTypes[ fields[idx].name()] = type;
}

QgsVectorLayer::EditorLayout QgsVectorLayer::editorLayout()
{
  return mEditorLayout;
}

void QgsVectorLayer::setEditorLayout( EditorLayout editorLayout )
{
  mEditorLayout = editorLayout;
}

QString QgsVectorLayer::editForm()
{
  return mEditForm;
}

void QgsVectorLayer::setEditForm( QString ui )
{
  mEditForm = ui;
}

void QgsVectorLayer::setAnnotationForm( const QString& ui )
{
  mAnnotationForm = ui;
}

QString QgsVectorLayer::editFormInit()
{
  return mEditFormInit;
}

void QgsVectorLayer::setEditFormInit( QString function )
{
  mEditFormInit = function;
}

QMap< QString, QVariant > &QgsVectorLayer::valueMap( int idx )
{
  const QgsFields &fields = pendingFields();

  // FIXME: throw an exception!?
  static QMap< QString, QVariant > invalidMap;
  if ( idx < 0 || idx >= fields.count() )
  {
    QgsDebugMsg( QString( "field %1 not found" ).arg( idx ) );
    return invalidMap;
  }
  QString fieldName = fields[idx].name();

  if ( !mValueMaps.contains( fieldName ) )
    mValueMaps[fieldName] = QMap<QString, QVariant>();

  return mValueMaps[fieldName];
}

QgsVectorLayer::RangeData &QgsVectorLayer::range( int idx )
{
  const QgsFields &fields = pendingFields();

  // FIXME: throw an exception!?
  static QgsVectorLayer::RangeData invalidRange;
  if ( idx < 0 || idx >= fields.count() )
  {
    QgsDebugMsg( QString( "field %1 not found" ).arg( idx ) );
    return invalidRange;
  }
  QString fieldName = fields[idx].name();

  if ( !mRanges.contains( fieldName ) )
    mRanges[fieldName] = RangeData();

  return mRanges[fieldName];
}

bool QgsVectorLayer::fieldEditable( int idx )
{
  const QgsFields &fields = pendingFields();
  if ( idx >= 0 && idx < fields.count() && mEditTypes.contains( fields[idx].name() ) )
    return mFieldEditables[ fields[idx].name() ];
  else
    return false;
}

void QgsVectorLayer::setFieldEditable( int idx, bool editable )
{
  const QgsFields &fields = pendingFields();
  if ( idx >= 0 && idx < fields.count() && mEditTypes.contains( fields[idx].name() ) )
    mFieldEditables[ fields[idx].name() ] = editable;
}

void QgsVectorLayer::addOverlay( QgsVectorOverlay* overlay )
{
  mOverlays.push_back( overlay );
}

void QgsVectorLayer::removeOverlay( const QString& typeName )
{
  for ( int i = mOverlays.size() - 1; i >= 0; --i )
  {
    if ( mOverlays.at( i )->typeName() == typeName )
    {
      mOverlays.removeAt( i );
    }
  }
}

void QgsVectorLayer::vectorOverlays( QList<QgsVectorOverlay*>& overlayList )
{
  overlayList = mOverlays;
}

QgsVectorOverlay* QgsVectorLayer::findOverlayByType( const QString& typeName )
{
  QList<QgsVectorOverlay*>::iterator it = mOverlays.begin();
  for ( ; it != mOverlays.end(); ++it )
  {
    if (( *it )->typeName() == typeName )
    {
      return *it;
    }
  }
  return 0; //not found
}


QgsFeatureRendererV2* QgsVectorLayer::rendererV2()
{
  return mRendererV2;
}

void QgsVectorLayer::setRendererV2( QgsFeatureRendererV2 *r )
{
  if ( !hasGeometryType() )
    return;

  if ( r != mRendererV2 )
  {
    if ( r )
      setUsingRendererV2( true );
    delete mRendererV2;
    mRendererV2 = r;
    mSymbolFeatureCounted = false;
    mSymbolFeatureCountMap.clear();
  }
}
bool QgsVectorLayer::isUsingRendererV2()
{
  return mUsingRendererV2;
}
void QgsVectorLayer::setUsingRendererV2( bool usingRendererV2 )
{
  if ( !hasGeometryType() )
    return;

  mUsingRendererV2 = usingRendererV2;
}



void QgsVectorLayer::beginEditCommand( QString text )
{
  undoStack()->beginMacro( text );
}

void QgsVectorLayer::endEditCommand()
{
  undoStack()->endMacro();
}

void QgsVectorLayer::destroyEditCommand()
{
  undoStack()->endMacro();
  undoStack()->undo();
}


void QgsVectorLayer::setCheckedState( int idx, QString checked, QString unchecked )
{
  const QgsFields &fields = pendingFields();
  if ( idx >= 0 && idx < fields.count() )
    mCheckedStates[ fields[idx].name()] = QPair<QString, QString>( checked, unchecked );
}

QPair<QString, QString> QgsVectorLayer::checkedState( int idx )
{
  const QgsFields &fields = pendingFields();
  if ( idx >= 0 && idx < fields.count() && mCheckedStates.contains( fields[idx].name() ) )
    return mCheckedStates[ fields[idx].name()];
  else
    return QPair<QString, QString>( "1", "0" );
}

int QgsVectorLayer::fieldNameIndex( const QString& fieldName ) const
{
  const QgsFields &theFields = pendingFields();

  for ( int idx = 0; idx < theFields.count(); ++idx )
  {
    if ( QString::compare( theFields[idx].name(), fieldName, Qt::CaseInsensitive ) == 0 )
    {
      return idx;
    }
  }
  return -1;
}

void QgsVectorLayer::addJoin( const QgsVectorJoinInfo& joinInfo )
{
  mJoinBuffer->addJoin( joinInfo );
  updateFields();
}

void QgsVectorLayer::checkJoinLayerRemove( QString theLayerId )
{
  removeJoin( theLayerId );
}

void QgsVectorLayer::removeJoin( const QString& joinLayerId )
{
  mJoinBuffer->removeJoin( joinLayerId );
  updateFields();
}

const QList< QgsVectorJoinInfo >& QgsVectorLayer::vectorJoins() const
{
  return mJoinBuffer->vectorJoins();
}

void QgsVectorLayer::updateFields()
{
  if ( !mDataProvider )
    return;

  mUpdatedFields = mDataProvider->fields();

  // added / removed fields
  if ( mEditBuffer )
    mEditBuffer->updateFields( mUpdatedFields );

  // joined fields
  if ( mJoinBuffer->containsJoins() )
    mJoinBuffer->updateFields( mUpdatedFields );
}


void QgsVectorLayer::createJoinCaches()
{
  if ( mJoinBuffer->containsJoins() )
  {
    mJoinBuffer->createJoinCaches();
  }
}

void QgsVectorLayer::uniqueValues( int index, QList<QVariant> &uniqueValues, int limit )
{
  uniqueValues.clear();
  if ( !mDataProvider )
  {
    return;
  }

  QgsFields::FieldOrigin origin = mUpdatedFields.fieldOrigin( index );

  if ( origin == QgsFields::OriginProvider ) //a provider field
  {
    return mDataProvider->uniqueValues( index, uniqueValues, limit );
  }
  else if ( origin == QgsFields::OriginJoin )
  {
    int sourceLayerIndex;
    const QgsVectorJoinInfo* join = mJoinBuffer->joinForFieldIndex( index, mUpdatedFields, sourceLayerIndex );
    Q_ASSERT( join );

    QgsVectorLayer* vl = dynamic_cast<QgsVectorLayer*>( QgsMapLayerRegistry::instance()->mapLayer( join->joinLayerId ) );
    Q_ASSERT( vl );

    return vl->dataProvider()->uniqueValues( sourceLayerIndex, uniqueValues, limit );
  }
  else if ( origin == QgsFields::OriginEdit )
  {
    // the layer is editable, but in certain cases it can still be avoided going through all features
    if ( mEditBuffer->mDeletedFeatureIds.isEmpty() && mEditBuffer->mAddedFeatures.isEmpty() && !mEditBuffer->mDeletedAttributeIds.contains( index ) && mEditBuffer->mChangedAttributeValues.isEmpty() )
    {
      return mDataProvider->uniqueValues( index, uniqueValues, limit );
    }

    // we need to go through each feature
    QgsAttributeList attList;
    attList << index;

    QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                          .setFlags( QgsFeatureRequest::NoGeometry )
                                          .setSubsetOfAttributes( attList ) );

    QgsFeature f;
    QVariant currentValue;
    QHash<QString, QVariant> val;
    while ( fit.nextFeature( f ) )
    {
      currentValue = f.attribute( index );
      val.insert( currentValue.toString(), currentValue );
      if ( limit >= 0 && val.size() >= limit )
      {
        break;
      }
    }

    uniqueValues = val.values();
    return;
  }

  Q_ASSERT_X( false, "QgsVectorLayer::uniqueValues()", "Unknown source of the field!" );
}

QVariant QgsVectorLayer::minimumValue( int index )
{
  if ( !mDataProvider )
  {
    return QVariant();
  }

  QgsFields::FieldOrigin origin = mUpdatedFields.fieldOrigin( index );

  if ( origin == QgsFields::OriginProvider ) //a provider field
  {
    return mDataProvider->minimumValue( index );
  }
  else if ( origin == QgsFields::OriginJoin )
  {
    int sourceLayerIndex;
    const QgsVectorJoinInfo* join = mJoinBuffer->joinForFieldIndex( index, mUpdatedFields, sourceLayerIndex );
    Q_ASSERT( join );

    QgsVectorLayer* vl = dynamic_cast<QgsVectorLayer*>( QgsMapLayerRegistry::instance()->mapLayer( join->joinLayerId ) );
    Q_ASSERT( vl );

    return vl->minimumValue( sourceLayerIndex );
  }
  else if ( origin == QgsFields::OriginEdit )
  {
    // the layer is editable, but in certain cases it can still be avoided going through all features
    if ( mEditBuffer->mDeletedFeatureIds.isEmpty() && mEditBuffer->mAddedFeatures.isEmpty() && !mEditBuffer->mDeletedAttributeIds.contains( index ) && mEditBuffer->mChangedAttributeValues.isEmpty() )
    {
      return mDataProvider->minimumValue( index );
    }

    // we need to go through each feature
    QgsAttributeList attList;
    attList << index;

    QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                          .setFlags( QgsFeatureRequest::NoGeometry )
                                          .setSubsetOfAttributes( attList ) );

    QgsFeature f;
    double minimumValue = std::numeric_limits<double>::max();
    double currentValue = 0;
    while ( fit.nextFeature( f ) )
    {
      currentValue = f.attribute( index ).toDouble();
      if ( currentValue < minimumValue )
      {
        minimumValue = currentValue;
      }
    }
    return QVariant( minimumValue );
  }

  Q_ASSERT_X( false, "QgsVectorLayer::minimumValue()", "Unknown source of the field!" );
  return QVariant();
}

QVariant QgsVectorLayer::maximumValue( int index )
{
  if ( !mDataProvider )
  {
    return QVariant();
  }

  QgsFields::FieldOrigin origin = mUpdatedFields.fieldOrigin( index );

  if ( origin == QgsFields::OriginProvider ) //a provider field
  {
    return mDataProvider->maximumValue( index );
  }
  else if ( origin == QgsFields::OriginJoin )
  {
    int sourceLayerIndex;
    const QgsVectorJoinInfo* join = mJoinBuffer->joinForFieldIndex( index, mUpdatedFields, sourceLayerIndex );
    Q_ASSERT( join );

    QgsVectorLayer* vl = dynamic_cast<QgsVectorLayer*>( QgsMapLayerRegistry::instance()->mapLayer( join->joinLayerId ) );
    Q_ASSERT( vl );

    return vl->maximumValue( index );
  }
  else if ( origin == QgsFields::OriginEdit )
  {
    // the layer is editable, but in certain cases it can still be avoided going through all features
    if ( mEditBuffer->mDeletedFeatureIds.isEmpty() &&
         mEditBuffer->mAddedFeatures.isEmpty() &&
         !mEditBuffer->mDeletedAttributeIds.contains( index ) &&
         mEditBuffer->mChangedAttributeValues.isEmpty() )
    {
      return mDataProvider->maximumValue( index );
    }

    // we need to go through each feature
    QgsAttributeList attList;
    attList << index;

    QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                          .setFlags( QgsFeatureRequest::NoGeometry )
                                          .setSubsetOfAttributes( attList ) );

    QgsFeature f;
    double maximumValue = -std::numeric_limits<double>::max();
    double currentValue = 0;
    while ( fit.nextFeature( f ) )
    {
      currentValue = f.attribute( index ).toDouble();
      if ( currentValue > maximumValue )
      {
        maximumValue = currentValue;
      }
    }
    return QVariant( maximumValue );
  }

  Q_ASSERT_X( false, "QgsVectorLayer::maximumValue()", "Unknown source of the field!" );
  return QVariant();
}

void QgsVectorLayer::stopRendererV2( QgsRenderContext& rendererContext, QgsSingleSymbolRendererV2* selRenderer )
{
  mRendererV2->stopRender( rendererContext );
  if ( selRenderer )
  {
    selRenderer->stopRender( rendererContext );
    delete selRenderer;
  }
}

void QgsVectorLayer::prepareLabelingAndDiagrams( QgsRenderContext& rendererContext, QgsAttributeList& attributes, bool& labeling )
{
  if ( !rendererContext.labelingEngine() )
    return;

  QSet<int> attrIndex;
  if ( rendererContext.labelingEngine()->prepareLayer( this, attrIndex, rendererContext ) )
  {
    QSet<int>::const_iterator attIt = attrIndex.constBegin();
    for ( ; attIt != attrIndex.constEnd(); ++attIt )
    {
      if ( !attributes.contains( *attIt ) )
      {
        attributes << *attIt;
      }
    }
    labeling = true;
  }

  if ( labeling )
  {
    // see if feature count limit is set for labeling
    QgsPalLayerSettings& palyr = rendererContext.labelingEngine()->layer( this->id() );
    if ( palyr.limitNumLabels && palyr.maxNumLabels > 0 )
    {
      QgsFeatureIterator fit = getFeatures( QgsFeatureRequest()
                                            .setFilterRect( rendererContext.extent() )
                                            .setSubsetOfAttributes( QgsAttributeList() ) );

      // total number of features that may be labeled
      QgsFeature f;
      int nFeatsToLabel = 0;
      while ( fit.nextFeature( f ) )
      {
        nFeatsToLabel++;
      }
      palyr.mFeaturesToLabel = nFeatsToLabel;
    }
  }

  //register diagram layers
  if ( mDiagramRenderer && mDiagramLayerSettings )
  {
    mDiagramLayerSettings->renderer = mDiagramRenderer;
    rendererContext.labelingEngine()->addDiagramLayer( this, mDiagramLayerSettings );
    //add attributes needed by the diagram renderer
    QList<int> att = mDiagramRenderer->diagramAttributes();
    QList<int>::const_iterator attIt = att.constBegin();
    for ( ; attIt != att.constEnd(); ++attIt )
    {
      if ( !attributes.contains( *attIt ) )
      {
        attributes << *attIt;
      }
    }
    //and the ones needed for data defined diagram positions
    if ( mDiagramLayerSettings->xPosColumn >= 0 && !attributes.contains( mDiagramLayerSettings->xPosColumn ) )
    {
      attributes << mDiagramLayerSettings->xPosColumn;
    }
    if ( mDiagramLayerSettings->yPosColumn >= 0 && !attributes.contains( mDiagramLayerSettings->yPosColumn ) )
    {
      attributes << mDiagramLayerSettings->yPosColumn;
    }
  }
}

void QgsVectorLayer::setDiagramLayerSettings( const QgsDiagramLayerSettings& s )
{
  if ( !mDiagramLayerSettings )
    mDiagramLayerSettings = new QgsDiagramLayerSettings();
  *mDiagramLayerSettings = s;
}

QString QgsVectorLayer::metadata()
{
  QString myMetadata = "<html><body>";
  myMetadata += "<table width=\"100%\">";

  //-------------

  myMetadata += "<tr class=\"glossy\"><td>";
  myMetadata += tr( "General:" );
  myMetadata += "</td></tr>";

  // data comment
  if ( !( dataComment().isEmpty() ) )
  {
    myMetadata += "<tr><td>";
    myMetadata += tr( "Layer comment: %1" ).arg( dataComment() );
    myMetadata += "</td></tr>";
  }

  //storage type
  myMetadata += "<tr><td>";
  myMetadata += tr( "Storage type of this layer: %1" ).arg( storageType() );
  myMetadata += "</td></tr>";

  // data source
  myMetadata += "<tr><td>";
  myMetadata += tr( "Source for this layer: %1" ).arg( publicSource() );
  myMetadata += "</td></tr>";

  //geom type

  QGis::GeometryType type = geometryType();

  if ( type < 0 || type > QGis::NoGeometry )
  {
    QgsDebugMsg( "Invalid vector type" );
  }
  else
  {
    QString typeString( QGis::vectorGeometryType( geometryType() ) );

    myMetadata += "<tr><td>";
    myMetadata += tr( "Geometry type of the features in this layer: %1" ).arg( typeString );
    myMetadata += "</td></tr>";
  }

  QgsAttributeList pkAttrList = pendingPkAttributesList();
  if ( !pkAttrList.isEmpty() )
  {
    myMetadata += "<tr><td>";
    myMetadata += tr( "Primary key attributes: " );
    foreach ( int idx, pkAttrList )
    {
      myMetadata += pendingFields()[ idx ].name() + " ";
    }
    myMetadata += "</td></tr>";
  }


  //feature count
  myMetadata += "<tr><td>";
  myMetadata += tr( "The number of features in this layer: %1" ).arg( featureCount() );
  myMetadata += "</td></tr>";
  //capabilities
  myMetadata += "<tr><td>";
  myMetadata += tr( "Editing capabilities of this layer: %1" ).arg( capabilitiesString() );
  myMetadata += "</td></tr>";

  //-------------

  QgsRectangle myExtent = extent();
  myMetadata += "<tr class=\"glossy\"><td>";
  myMetadata += tr( "Extents:" );
  myMetadata += "</td></tr>";
  //extents in layer cs  TODO...maybe make a little nested table to improve layout...
  myMetadata += "<tr><td>" + tr( "In layer spatial reference system units : " );

  // Try to be a bit clever over what number format we use for the
  // extents. Some people don't like it using scientific notation when the
  // numbers get large, but for small numbers this is the more practical
  // option (so we can't force the format to 'f' for all values).
  // The scheme:
  // - for all numbers with more than 5 digits, force non-scientific notation
  // and 2 digits after the decimal point.
  // - for all smaller numbers let the OS decide which format to use (it will
  // generally use non-scientific unless the number gets much less than 1).

  if ( !myExtent.isEmpty() )
  {
    QString xMin, yMin, xMax, yMax;
    double changeoverValue = 99999; // The 'largest' 5 digit number
    if ( qAbs( myExtent.xMinimum() ) > changeoverValue )
    {
      xMin = QString( "%1" ).arg( myExtent.xMinimum(), 0, 'f', 2 );
    }
    else
    {
      xMin = QString( "%1" ).arg( myExtent.xMinimum() );
    }
    if ( qAbs( myExtent.yMinimum() ) > changeoverValue )
    {
      yMin = QString( "%1" ).arg( myExtent.yMinimum(), 0, 'f', 2 );
    }
    else
    {
      yMin = QString( "%1" ).arg( myExtent.yMinimum() );
    }
    if ( qAbs( myExtent.xMaximum() ) > changeoverValue )
    {
      xMax = QString( "%1" ).arg( myExtent.xMaximum(), 0, 'f', 2 );
    }
    else
    {
      xMax = QString( "%1" ).arg( myExtent.xMaximum() );
    }
    if ( qAbs( myExtent.yMaximum() ) > changeoverValue )
    {
      yMax = QString( "%1" ).arg( myExtent.yMaximum(), 0, 'f', 2 );
    }
    else
    {
      yMax = QString( "%1" ).arg( myExtent.yMaximum() );
    }

    myMetadata += tr( "xMin,yMin %1,%2 : xMax,yMax %3,%4" )
                  .arg( xMin ).arg( yMin ).arg( xMax ).arg( yMax );
  }
  else
  {
    myMetadata += tr( "unknown extent" );
  }

  myMetadata += "</td></tr>";

  //extents in project cs

  try
  {
#if 0
    // TODO: currently disabled, will revisit later [MD]
    QgsRectangle myProjectedExtent = coordinateTransform->transformBoundingBox( extent() );
    myMetadata += "<tr><td>";
    myMetadata += tr( "In project spatial reference system units : " )
                  + tr( "xMin,yMin %1,%2 : xMax,yMax %3,%4" )
                  .arg( myProjectedExtent.xMinimum() )
                  .arg( myProjectedExtent.yMinimum() )
                  .arg( myProjectedExtent.xMaximum() )
                  .arg( myProjectedExtent.yMaximum() );
    myMetadata += "</td></tr>";
#endif

    //
    // Display layer spatial ref system
    //
    myMetadata += "<tr class=\"glossy\"><td>";
    myMetadata += tr( "Layer Spatial Reference System:" );
    myMetadata += "</td></tr>";
    myMetadata += "<tr><td>";
    myMetadata += crs().toProj4().replace( QRegExp( "\"" ), " \"" );
    myMetadata += "</td></tr>";

    //
    // Display project (output) spatial ref system
    //
#if 0
    // TODO: disabled for now, will revisit later [MD]
    myMetadata += "<tr><td bgcolor=\"gray\">";
    myMetadata += tr( "Project (Output) Spatial Reference System:" );
    myMetadata += "</td></tr>";
    myMetadata += "<tr><td>";
    myMetadata += coordinateTransform->destCRS().toProj4().replace( QRegExp( "\"" ), " \"" );
    myMetadata += "</td></tr>";
#endif
  }
  catch ( QgsCsException &cse )
  {
    Q_UNUSED( cse );
    QgsDebugMsg( cse.what() );

    myMetadata += "<tr><td>";
    myMetadata += tr( "In project spatial reference system units : " )
                  + tr( "(Invalid transformation of layer extents)" );
    myMetadata += "</td></tr>";

  }

#if 0
  //
  // Add the info about each field in the attribute table
  //
  myMetadata += "<tr class=\"glossy\"><td>";
  myMetadata += tr( "Attribute field info:" );
  myMetadata += "</td></tr>";
  myMetadata += "<tr><td>";

  // Start a nested table in this trow
  myMetadata += "<table width=\"100%\">";
  myMetadata += "<tr><th>";
  myMetadata += tr( "Field" );
  myMetadata += "</th>";
  myMetadata += "<th>";
  myMetadata += tr( "Type" );
  myMetadata += "</th>";
  myMetadata += "<th>";
  myMetadata += tr( "Length" );
  myMetadata += "</th>";
  myMetadata += "<th>";
  myMetadata += tr( "Precision" );
  myMetadata += "</th>";
  myMetadata += "<th>";
  myMetadata += tr( "Comment" );
  myMetadata += "</th>";

  //get info for each field by looping through them
  const QgsFieldMap& myFields = pendingFields();
  for ( QgsFieldMap::const_iterator it = myFields.begin(); it != myFields.end(); ++it )
  {
    const QgsField& myField = *it;

    myMetadata += "<tr><td>";
    myMetadata += myField.name();
    myMetadata += "</td>";
    myMetadata += "<td>";
    myMetadata += myField.typeName();
    myMetadata += "</td>";
    myMetadata += "<td>";
    myMetadata += QString( "%1" ).arg( myField.length() );
    myMetadata += "</td>";
    myMetadata += "<td>";
    myMetadata += QString( "%1" ).arg( myField.precision() );
    myMetadata += "</td>";
    myMetadata += "<td>";
    myMetadata += QString( "%1" ).arg( myField.comment() );
    myMetadata += "</td></tr>";
  }

  //close field list
  myMetadata += "</table>"; //end of nested table
#endif

  myMetadata += "</td></tr>"; //end of stats container table row
  //
  // Close the table
  //

  myMetadata += "</table>";

  myMetadata += "</body></html>";
  return myMetadata;
}

QgsVectorLayer::ValueRelationData &QgsVectorLayer::valueRelation( int idx )
{
  const QgsFields &fields = pendingFields();

  // FIXME: throw an exception!?
  static QgsVectorLayer::ValueRelationData invalidData;
  if ( idx < 0 || idx >= fields.count() )
  {
    QgsDebugMsg( QString( "field %1 not found" ).arg( idx ) );
    return invalidData;
  }
  QString fieldName = fields[idx].name();

  if ( !mValueRelations.contains( fieldName ) )
  {
    mValueRelations[fieldName] = ValueRelationData();
  }

  return mValueRelations[fieldName];
}

QList<QgsAttributeEditorElement*> &QgsVectorLayer::attributeEditorElements()
{
  return mAttributeEditorElements;
}

void QgsVectorLayer::clearAttributeEditorWidgets()
{
  mAttributeEditorElements.clear();
}

QDomElement QgsAttributeEditorContainer::toDomElement( QDomDocument& doc ) const
{
  QDomElement elem = doc.createElement( "attributeEditorContainer" );
  elem.setAttribute( "name", mName );
  for ( QList< QgsAttributeEditorElement* >::const_iterator it = mChildren.begin(); it != mChildren.end(); ++it )
  {
    elem.appendChild(( *it )->toDomElement( doc ) );
  }
  return elem;
}


void QgsAttributeEditorContainer::addChildElement( QgsAttributeEditorElement *widget )
{
  mChildren.append( widget );
}

QDomElement QgsAttributeEditorField::toDomElement( QDomDocument& doc ) const
{
  QDomElement elem = doc.createElement( "attributeEditorField" );
  elem.setAttribute( "name", mName );
  elem.setAttribute( "index", mIdx );
  return elem;
}
