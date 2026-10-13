#pragma once

#include "config.hh"

#include <QNetworkReply>
#include <QObject>
#include <QJsonObject>

class AnkiConnector: public QObject
{
  Q_OBJECT

public:
  explicit AnkiConnector( QObject * parent, const Config::Class & cfg );

  /// autoSend: sent without user action; duplicates in the deck are skipped silently.
  void sendToAnki( const QString & word,
                   QString text,
                   const QString & sentence,
                   const QJsonObject & audio = QJsonObject(),
                   bool autoSend             = false );
  void ankiSearch( const QString & word );

private:
  QNetworkAccessManager * mgr;
  const Config::Class & cfg;
  void postToAnki( const QString & postData, bool autoSend = false );
  static constexpr auto transfer_timeout = 3000;

public:
signals:
  void errorText( const QString & );
private slots:
  void finishedSlot( QNetworkReply * reply );
};
