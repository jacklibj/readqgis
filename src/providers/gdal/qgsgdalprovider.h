/***************************************************************************
      qgsgdalprovider.h  -  QGIS Data provider for
                           GDAL rasters
                             -------------------
    begin                : November, 2010
    copyright            : (C) 2010 by Radim Blazek
    email                : radim dot blazek at gmail dot com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QGSGDALPROVIDER_H
#define QGSGDALPROVIDER_H

#include "qgscoordinatereferencesystem.h"
#include "qgsdataitem.h"
#include "qgsrasterdataprovider.h"
#include "qgsgdalproviderbase.h"
#include "qgsrectangle.h"
#include "qgscolorrampshader.h"
#include "qgsrasterbandstats.h"

#include <QString>
#include <QStringList>
#include <QDomElement>
#include <QMap>
#include <QVector>

class QgsRasterPyramid;

/** \ingroup core
 * A call back function for showing progress of gdal operations.
 */
int CPL_STDCALL progressCallback( double dfComplete,
                                  const char *pszMessage,
                                  void * pProgressArg );


class QgsCoordinateTransform;

/**

  \brief Data provider for GDAL layers.

  This provider implements the interface defined in the QgsDataProvider class
  to provide access to spatial data residing in a GDAL layers.

*/
class QgsGdalProvider : public QgsRasterDataProvider, QgsGdalProviderBase
{
    Q_OBJECT

  public:
    /**
    * Constructor for the provider.
    *
    * \param   uri   HTTP URL of the Web Server.  If needed a proxy will be used
    *                otherwise we contact the host directly.
    *
    */
    QgsGdalProvider( QString const & uri = 0, bool update = false );

    /** Create invalid provider with error */
    QgsGdalProvider( QString const & uri, QgsError error );

    //! Destructor
    ~QgsGdalProvider();

    QgsRasterInterface * clone() const;

    /** \brief   Renders the layer as an image
     */
    QImage* draw( QgsRectangle  const & viewExtent, int pixelWidth, int pixelHeight );

    /** return a provider name

    Essentially just returns the provider key.  Should be used to build file
    dialogs so that providers can be shown with their supported types. Thus
    if more than one provider supports a given format, the user is able to
    select a specific provider to open that file.

    @note

    Instead of being pure virtual, might be better to generalize this
    behavior and presume that none of the sub-classes are going to do
    anything strange with regards to their name or description?

    */
    QString name() const;


    /** return description

    Return a terse string describing what the provider is.

    @note

    Instead of being pure virtual, might be better to generalize this
    behavior and presume that none of the sub-classes are going to do
    anything strange with regards to their name or description?

    */
    QString description() const;

    /*! Get the QgsCoordinateReferenceSystem for this layer
     * @note Must be reimplemented by each provider.
     * If the provider isn't capable of returning
     * its projection an empty srs will be return, ti will return 0
     */
    virtual QgsCoordinateReferenceSystem crs();

    /** Return the extent for this data layer
    */
    virtual QgsRectangle extent();

    /**Returns true if layer is valid
    */
    bool isValid();

    QMap<int, QVariant> identify( const QgsPoint & thePoint, IdentifyFormat theFormat, const QgsRectangle &theExtent = QgsRectangle(), int theWidth = 0, int theHeight = 0 );

    /**
     * \brief   Returns the caption error text for the last error in this provider
     *
     * If an operation returns 0 (e.g. draw()), this function
     * returns the text of the error associated with the failure.
     * Interactive users of this provider can then, for example,
     * call a QMessageBox to display the contents.
     */
    QString lastErrorTitle();

    /**
     * \brief   Returns the verbose error text for the last error in this provider
     *
     * If an operation returns 0 (e.g. draw()), this function
     * returns the text of the error associated with the failure.
     * Interactive users of this provider can then, for example,
     * call a QMessageBox to display the contents.
     */

    QString lastError();

    /** Returns a bitmask containing the supported capabilities
        Note, some capabilities may change depending on which
        sublayers are visible on this provider, so it may
        be prudent to check this value per intended operation.
      */
    int capabilities() const;

    QGis::DataType dataType( int bandNo ) const;
    QGis::DataType srcDataType( int bandNo ) const;

    QGis::DataType dataTypeFormGdal( int theGdalDataType ) const;

    int bandCount() const;

    int colorInterpretation( int bandNo ) const;

    int xBlockSize() const;
    int yBlockSize() const;

    int xSize() const;
    int ySize() const;

    /**Reimplemented from QgsRasterDataProvider to bypass second resampling (more efficient for local file based sources)*/
    QgsRasterBlock *block( int theBandNo, const QgsRectangle &theExtent, int theWidth, int theHeight );

    void readBlock( int bandNo, int xBlock, int yBlock, void *data );
    void readBlock( int bandNo, QgsRectangle  const & viewExtent, int width, int height, void *data );

    //void * readBlock( int bandNo, QgsRectangle  const & extent, int width, int height );

    //bool srcHasNoDataValue( int bandNo ) const;
    //double noDataValue() const;
    void computeMinMax( int bandNo ) const;
    double minimumValue( int bandNo ) const;
    double maximumValue( int bandNo ) const;

    QList<QgsColorRampShader::ColorRampItem> colorTable( int bandNo )const;

    /**
     * Get metadata in a format suitable for feeding directly
     * into a subset of the GUI raster properties "Metadata" tab.
     */
    QString metadata();

    /** \brief Returns the sublayers of this layer - Useful for providers that manage their own layers, such as WMS */
    QStringList subLayers() const;
    static QStringList subLayers( GDALDatasetH dataset );

    bool hasStatistics( int theBandNo,
                        int theStats = QgsRasterBandStats::All,
                        const QgsRectangle & theExtent = QgsRectangle(),
                        int theSampleSize = 0 );

    QgsRasterBandStats bandStatistics( int theBandNo,
                                       int theStats = QgsRasterBandStats::All,
                                       const QgsRectangle & theExtent = QgsRectangle(),
                                       int theSampleSize = 0 );

    bool hasHistogram( int theBandNo,
                       int theBinCount = 0,
                       double theMinimum = std::numeric_limits<double>::quiet_NaN(),
                       double theMaximum = std::numeric_limits<double>::quiet_NaN(),
                       const QgsRectangle & theExtent = QgsRectangle(),
                       int theSampleSize = 0,
                       bool theIncludeOutOfRange = false );

    QgsRasterHistogram histogram( int theBandNo,
                                  int theBinCount = 0,
                                  double theMinimum = std::numeric_limits<double>::quiet_NaN(),
                                  double theMaximum = std::numeric_limits<double>::quiet_NaN(),
                                  const QgsRectangle & theExtent = QgsRectangle(),
                                  int theSampleSize = 0,
                                  bool theIncludeOutOfRange = false );

    QString buildPyramids( const QList<QgsRasterPyramid> & theRasterPyramidList,
                           const QString & theResamplingMethod = "NEAREST",
                           RasterPyramidsFormat theFormat = PyramidsGTiff,
                           const QStringList & theCreateOptions = QStringList() );
    QList<QgsRasterPyramid> buildPyramidList( QList<int> overviewList = QList<int>() );

    /** \brief Close data set and release related data */
    void closeDataset();

    /** Emit a signal to notify of the progress event. */
    void emitProgress( int theType, double theProgress, QString theMessage );
    void emitProgressUpdate( int theProgress );

    static QMap<QString, QString> supportedMimes();

    /** Creates a new dataset with mDataSourceURI
        @return true in case of success*/
    /*
    bool create( const QString& format, int nBands,
                 QGis::DataType type,
                 int width, int height, double* geoTransform,
                 const QgsCoordinateReferenceSystem& crs,
                 QStringList createOptions = QStringList() );
    */

    /**Writes into the provider datasource*/
    bool write( void* data, int band, int width, int height, int xOffset, int yOffset );

    bool setNoDataValue( int bandNo, double noDataValue );

    /**Returns the formats supported by create()*/
    QStringList createFormats() const;

    /**Remove dataset*/
    bool remove();

    QString validateCreationOptions( const QStringList& createOptions, QString format );
    QString validatePyramidsCreationOptions( RasterPyramidsFormat pyramidsFormat,
        const QStringList & theConfigOptions, const QString & fileFormat );

  signals:
    void statusChanged( QString );

  private:
    // update mode
    bool mUpdate;

    // initialize CRS from wkt
    bool crsFromWkt( const char *wkt );

    /**Do some initialisation on the dataset (e.g. handling of south-up datasets)*/
    void initBaseDataset();

    /**
    * Flag indicating if the layer data source is a valid layer
    */
    bool mValid;

    /** \brief Whether this raster has overviews / pyramids or not */
    bool mHasPyramids;

    /** \brief Gdal data types used to represent data in in QGIS,
               may be longer than source data type to keep nulls
               indexed from 0
     */
    QList<int>mGdalDataType;

    QgsRectangle mExtent;
    int mWidth;
    int mHeight;
    int mXBlockSize;
    int mYBlockSize;

    mutable QList<bool> mMinMaxComputed;

    // List of estimated min values, index 0 for band 1
    mutable QList<double> mMinimum;

    // List of estimated max values, index 0 for band 1
    mutable QList<double> mMaximum;

    /** \brief Pointer to the gdaldataset */
    GDALDatasetH mGdalBaseDataset;

    /** \brief Pointer to the gdaldataset (possibly warped vrt) */
    GDALDatasetH mGdalDataset;

    /** \brief Values for mapping pixel to world coordinates. Contents of this array are the same as the GDAL adfGeoTransform */
    double mGeoTransform[6];

    QgsCoordinateReferenceSystem mCrs;

    QList<QgsRasterPyramid> mPyramidList;

    /** \brief sublayers list saved for subsequent access */
    QStringList mSubLayers;

};

#endif

