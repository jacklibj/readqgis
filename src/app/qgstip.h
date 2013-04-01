/***************************************************************************
    qgstip.h
    ---------------------
    begin                : February 2011
    copyright            : (C) 2011 by Tim Sutton
    email                : tim at linfiniti dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#ifndef QGSTIP
#define QGSTIP

#include <QObject>
#include <QString>

/** \ingroup app
* \brief An QgsTip represents a tip generated by the
* QgsTipFactory factory class to serve up tips to the user.
* Tips can be generic, in which case they make no mention of
* gui dialogs etc, or gui-specific in which case they may allude
* to features of the graphical user interface.
* @see also QgsTipOfTheDay, QgsTipFactory
*/

class QgsTip
{
  public:
    /** Constructor */
    QgsTip() {};
    /**Destructor */
    ~QgsTip() {};
    //
    // Accessors
    //
    /** Get the tip title */
    QString title() {return mTitle;};
    /** Get the tip content */
    QString content() {return mContent;}

    //
    // Mutators
    //
    /** Set the tip title */
    void setTitle( QString theTitle ) {mTitle = theTitle;};
    /** Set the tip content*/
    void setContent( QString theContent ) {mContent = theContent;};
  private:
    QString mTitle;
    QString mContent;
};

#endif //QGSTIP

