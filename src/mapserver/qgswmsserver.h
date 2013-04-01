/***************************************************************************
                              qgswmsserver.h
                              -------------------
  begin                : May 14, 2006
  copyright            : (C) 2006 by Marco Hugentobler
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

#ifndef QGSWMSSERVER_H
#define QGSWMSSERVER_H

#include <QDomDocument>
#include <QMap>
#include <QPair>
#include <QString>
#include <map>

class QgsCoordinateReferenceSystem;
class QgsComposerLayerItem;
class QgsComposerLegendItem;
class QgsComposition;
class QgsConfigParser;
class QgsFeatureRendererV2;
class QgsMapLayer;
class QgsMapRenderer;
class QgsPoint;
class QgsRasterLayer;
class QgsRasterRenderer;
class QgsRectangle;
class QgsRenderContext;
class QgsVectorLayer;
class QgsSymbol;
class QColor;
class QFile;
class QFont;
class QImage;
class QPaintDevice;
class QPainter;

/**This class handles all the wms server requests. The parameters and values have to be passed in the form of
a map<QString, QString>. This map is usually generated by a subclass of QgsWMSRequestHandler, which makes QgsWMSServer
independent from any server side technology*/

class QgsWMSServer
{
  public:
    /**Constructor. Takes parameter map and a pointer to a renderer object (does not take ownership)*/
    QgsWMSServer( QMap<QString, QString> parameters, QgsMapRenderer* renderer );
    ~QgsWMSServer();
    /**Returns an XML file with the capabilities description (as described in the WMS specs)
        @param version WMS version (1.1.1 or 1.3.0)
        @param fullProjectInformation If true: add extended project information (does not validate against WMS schema)*/
    QDomDocument getCapabilities( QString version = "1.3.0", bool fullProjectInformation = false );
    /**Returns the map legend as an image (or a null pointer in case of error). The caller takes ownership
    of the image object*/
    QImage* getLegendGraphics();
    /**Returns the map as an image (or a null pointer in case of error). The caller takes ownership
    of the image object)*/
    QImage* getMap();
    /**Returns an SLD file with the style of the requested layer. Exception is raised in case of troubles :-)*/
    QDomDocument getStyle();

    /**Returns printed page as binary
      @param formatString out: format of the print output (e.g. pdf, svg, png, ...)
      @return printed page as binary or 0 in case of error*/
    QByteArray* getPrint( const QString& formatString );

    /**Creates an xml document that describes the result of the getFeatureInfo request.
       @return 0 in case of success*/
    int getFeatureInfo( QDomDocument& result, QString version = "1.3.0" );

    /**Sets configuration parser for administration settings. Does not take ownership*/
    void setAdminConfigParser( QgsConfigParser* parser ) { mConfigParser = parser; }

  private:
    /**Don't use the default constructor*/
    QgsWMSServer();

    /**Initializes WMS layers and configures mMapRendering.
      @param layersList out: list with WMS layer names
      @param stylesList out: list with WMS style names
      @param layerIdList out: list with QGIS layer ids
      @return image configured together with mMapRenderer (or 0 in case of error). The calling function takes ownership of the image*/
    QImage* initializeRendering( QStringList& layersList, QStringList& stylesList, QStringList& layerIdList );

    /**Creates a QImage from the HEIGHT and WIDTH parameters
     @param width image width (or -1 if width should be taken from WIDTH wms parameter)
     @param height image height (or -1 if height should be taken from HEIGHT wms parameter)
     @return 0 in case of error*/
    QImage* createImage( int width = -1, int height = -1 ) const;
    /**Configures mMapRenderer to the parameters
     HEIGHT, WIDTH, BBOX, CRS.
     @param paintDevice the device that is used for painting (for dpi)
     @return 0 in case of success*/
    int configureMapRender( const QPaintDevice* paintDevice ) const;
    /**Reads the layers and style lists from the parameters LAYERS and STYLES
     @return 0 in case of success*/
    int readLayersAndStyles( QStringList& layersList, QStringList& stylesList ) const;
    /**If the parameter SLD exists, mSLDParser is configured appropriately. The lists are
    set to the layer and style names according to the SLD
    @return 0 in case of success*/
    int initializeSLDParser( QStringList& layersList, QStringList& stylesList );
    /**Calculates the location of a feature info point in layer coordinates
     @param i pixel x-coordinate
    @param j pixel y-coordinate
    @param layerCoords calculated layer coordinates are assigned to this point
    @return 0 in case of success*/
    int infoPointToLayerCoordinates( int i, int j, QgsPoint* layerCoords, QgsMapRenderer* mapRender,
                                     QgsMapLayer* layer ) const;
    /**Appends feature info xml for the layer to the layer element of the feature info dom document
    @param featureBBox the bounding box of the selected features in output CRS
    @return 0 in case of success*/
    int featureInfoFromVectorLayer( QgsVectorLayer* layer, const QgsPoint* infoPoint, int nFeatures, QDomDocument& infoDocument, QDomElement& layerElement, QgsMapRenderer* mapRender,
                                    QgsRenderContext& renderContext, QString version, QgsRectangle* featureBBox = 0 ) const;
    /**Appends feature info xml for the layer to the layer element of the dom document*/
    int featureInfoFromRasterLayer( QgsRasterLayer* layer, const QgsPoint* infoPoint, QDomDocument& infoDocument, QDomElement& layerElement, QString version ) const;

    /**Creates a layer set and returns a stringlist with layer ids that can be passed to a QgsMapRenderer. Usually used in conjunction with readLayersAndStyles
       @param scaleDenominator Filter out layer if scale based visibility does not match (or use -1 if no scale restriction)*/
    QStringList layerSet( const QStringList& layersList, const QStringList& stylesList, const QgsCoordinateReferenceSystem& destCRS, double scaleDenominator = -1 ) const;

    //helper functions for GetLegendGraphics
    /**Draws layer item and subitems
       @param p painter if the item should be drawn, if 0 the size parameters are calculated only
       @param maxTextWidth Includes boxSpace (on the right side). If p==0: maximumTextWidth is calculated, if p: maxTextWidth parameter is used for rendering
       @param maxSymbolWidth Includes boxSpace and iconLabelSpace. If p==0: maximum Symbol width is calculated, if p: maxSymbolWidth is input parameter
      */
    void drawLegendLayerItem( QgsComposerLayerItem* item, QPainter* p, double& maxTextWidth, double& maxSymbolWidth, double& currentY, const QFont& layerFont,
                              const QColor& layerFontColor, const QFont& itemFont, const QColor&  itemFontColor, double boxSpace, double layerSpace,
                              double layerTitleSpace, double symbolSpace, double iconLabelSpace, double symbolWidth, double symbolHeight, double fontOversamplingFactor, double dpi ) const;
    /**Draws a (old generation) symbol. Optionally, maxHeight is adapted (e.g. for large point markers) */
    void drawLegendSymbol( QgsComposerLegendItem* item, QPainter* p, double boxSpace, double currentY, double& symbolWidth, double& symbolHeight,
                           double layerOpacity, double dpi, double yDownShift ) const;
    void drawPointSymbol( QPainter* p, QgsSymbol* s, double boxSpace, double currentY, double& symbolWidth, double& symbolHeight, double layerOpacity, double dpi ) const;
    void drawLineSymbol( QPainter* p, QgsSymbol* s, double boxSpace, double currentY, double symbolWidth, double symbolHeight, double layerOpacity, double yDownShift ) const;
    void drawPolygonSymbol( QPainter* p, QgsSymbol* s, double boxSpace, double currentY, double symbolWidth, double symbolHeight, double layerOpacity, double yDownShift ) const;
    void drawLegendSymbolV2( QgsComposerLegendItem* item, QPainter* p, double boxSpace, double currentY, double& symbolWidth, double& symbolHeight, double dpi, double yDownShift ) const;
    void drawRasterSymbol( QgsComposerLegendItem* item, QPainter* p, double boxSpace, double currentY, double symbolWidth, double symbolHeight, double yDownShift ) const;

    /**Read legend parameter from the request or from the first print composer in the project*/
    void legendParameters( double mmToPixelFactor, double fontOversamplingFactor, double& boxSpace, double& layerSpace, double& layerTitleSpace,
                           double& symbolSpace, double& iconLabelSpace, double& symbolWidth, double& symbolHeight, QFont& layerFont, QFont& itemFont, QColor& layerFontColor, QColor& itemFontColor );

    QImage* printCompositionToImage( QgsComposition* c ) const;

    /**Apply filter (subset) strings from the request to the layers. Example: '&FILTER=<layer1>:"AND property > 100",<layer2>:"AND bla = 'hallo!'" '
       @return a map with the original filters ( layer id / filter string )*/
    QMap<QString, QString> applyRequestedLayerFilters( const QStringList& layerList ) const;
    /**Restores the original layer filters*/
    void restoreLayerFilters( const QMap < QString, QString >& filterMap ) const;
    /**Tests if a filter sql string is allowed (safe)
      @return true in case of success, false if string seems unsafe*/
    bool testFilterStringSafety( const QString& filter ) const;

    /**Select vector features with ids specified in parameter SELECTED, e.g. ...&SELECTED=layer1:1,2,9;layer2:3,5,10&...
      @return list with layer ids where selections have been created*/
    QStringList applyFeatureSelections( const QStringList& layerList ) const;
    /**Clear all feature selections in the given layers*/
    void clearFeatureSelections( const QStringList& layerIds ) const;

    /**Applies opacity on layer/group level*/
    void applyOpacities( const QStringList& layerList, QList< QPair< QgsVectorLayer*, QgsFeatureRendererV2*> >& vectorRenderers,
                         QList< QPair< QgsVectorLayer*, unsigned int> >& vectorOld,
                         QList< QPair< QgsRasterLayer*, QgsRasterRenderer* > >& rasterRenderers,
                         QList< QPair< QgsVectorLayer*, double > >& labelTransparencies,
                         QList< QPair< QgsVectorLayer*, double > >& labelBufferTransparencies
                       );

    /**Restore original opacities*/
    void restoreOpacities( QList< QPair <QgsVectorLayer*, QgsFeatureRendererV2*> >& vectorRenderers,
                           QList< QPair <QgsVectorLayer*, unsigned int> >& vectorOld,
                           QList< QPair < QgsRasterLayer*, QgsRasterRenderer* > >& rasterRenderers,
                           QList< QPair< QgsVectorLayer*, double > >& labelTransparencies,
                           QList< QPair< QgsVectorLayer*, double > >& labelBufferTransparencies );

    void appendFormats( QDomDocument &doc, QDomElement &elem, const QStringList &formats );

    /**Checks WIDTH/HEIGHT values agains MaxWidth and MaxHeight
      @return true if width/height values are okay*/
    bool checkMaximumWidthHeight() const;

    /**Get service address from REQUEST_URI if not specified in the configuration*/
    QString serviceUrl() const;

    /**Add '<?xml version="1.0" ?>'. Some clients need an xml declaration (though it is not strictly required)*/
    void addXMLDeclaration( QDomDocument& doc ) const;

    /**Converts a feature info xml document to SIA2045 norm*/
    void convertFeatureInfoToSIA2045( QDomDocument& doc );

    /**Map containing the WMS parameters*/
    QMap<QString, QString> mParameterMap;
    QgsConfigParser* mConfigParser;
    QgsMapRenderer* mMapRenderer;
};

#endif
