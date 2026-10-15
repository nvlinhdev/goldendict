#include "ankiconnector.hh"
#include "utils.hh"
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QPointer>
#include <QTimer>

namespace {
// Auto-sent notes that failed because Anki wasn't running. Shared by all the views
// and kept in the config dir, so looked-up words aren't lost until Anki is started.
QStringList queue;
bool queueLoaded = false;
QPointer< QTimer > retryTimer;
constexpr auto retryInterval = 60 * 1000;
const auto postDataAttribute = QNetworkRequest::Attribute( QNetworkRequest::User + 1 );

QString queueFile()
{
  return Config::getConfigDir() + "anki-queue.json";
}

void saveQueue()
{
  QFile file( queueFile() );
  if ( queue.isEmpty() ) {
    file.remove();
  }
  else if ( file.open( QIODevice::WriteOnly ) ) {
    file.write( QJsonDocument( QJsonArray::fromStringList( queue ) ).toJson() );
  }
}
} // namespace

QString markTargetWord( const QString & sentence, const QString & word )
{
  // Mark whole words only: "cat" in "The cat is in the category."
  // must produce "The <b>cat</b> is in the category.".
  // Note: Qt's replacement string supports \1..\99 (capture groups) but not \0,
  // hence the capturing group around the whole pattern.
  QString escapedWord = QRegularExpression::escape( word );
  QRegularExpression re( QString( R"(((?<!\w)%1(?!\w)))" ).arg( escapedWord ),
                         QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption );
  QString result = sentence;
  result.replace( re, "<b>\\1</b>" );
  if ( result == sentence ) {
    // Japanese/Chinese text has no word boundaries (\w matches kanji/kana):
    // "すき焼き" in "昨日すき焼きを食べました" must produce "昨日<b>すき焼き</b>を食べました".
    result.replace( word, "<b>" + word + "</b>", Qt::CaseInsensitive );
  }
  return result;
}

AnkiConnector::AnkiConnector( QObject * parent, const Config::Class & _cfg ):
  QObject{ parent },
  cfg( _cfg )
{
  mgr = new QNetworkAccessManager( this );
  connect( mgr, &QNetworkAccessManager::finished, this, &AnkiConnector::finishedSlot );

  if ( !queueLoaded ) {
    queueLoaded = true;
    QFile file( queueFile() );
    if ( file.open( QIODevice::ReadOnly ) ) {
      for ( const auto & postData : QJsonDocument::fromJson( file.readAll() ).array() ) {
        queue.append( postData.toString() );
      }
    }
  }
  scheduleRetry();
}

void AnkiConnector::flushQueue()
{
  // posted without removing: a reply from Anki removes it, a failure keeps it for the next retry
  for ( const auto & postData : std::as_const( queue ) ) {
    postToAnki( postData, true );
  }
  scheduleRetry();
}

void AnkiConnector::scheduleRetry()
{
  // one timer for all the views; it goes away with its view, the next view or failure makes a new one
  if ( !queue.isEmpty() && !retryTimer ) {
    retryTimer = new QTimer( this );
    connect( retryTimer, &QTimer::timeout, this, &AnkiConnector::flushQueue );
    retryTimer->start( retryInterval );
  }
  else if ( queue.isEmpty() && retryTimer ) {
    retryTimer->deleteLater();
  }
}

void AnkiConnector::sendToAnki(
  const QString & word, QString text, const QString & sentence, const QJsonObject & audio, bool autoSend )
{
  if ( word.isEmpty() ) {
    emit this->errorText( tr( "Anki: can't create a card without a word" ) );
    return;
  }

  // Anki doesn't understand the newline character, so it should be escaped.
  text = text.replace( "\n", "<br>" );

  QJsonObject params;
  QJsonObject note;
  QJsonObject fields;

  fields.insert( cfg.preferences.ankiConnectServer.word, word );
  fields.insert( cfg.preferences.ankiConnectServer.text, text );

  if ( !cfg.preferences.ankiConnectServer.sentence.isEmpty() ) {
    QString sentence_changed = markTargetWord( sentence, word );
    fields.insert( cfg.preferences.ankiConnectServer.sentence, sentence_changed );
  }

  QJsonObject options;
  options.insert( "allowDuplicate", !autoSend );
  if ( autoSend ) {
    options.insert( "duplicateScope", "deck" );
  }

  QJsonArray audioArray;
  if ( !audio.isEmpty() && cfg.preferences.ankiConnectServer.addAudio ) {
    audioArray.append( audio );
  }

  QString postTemplate = R"anki({
      "action": "addNote",
      "version": 6,
      "params": {
          "note": {
              "deckName": %1,
              "modelName": %2,
              "fields": %3,
              "audio": %4,
              "options": %5,
              "tags": []
          }
      }
  })anki";

  QString postData = postTemplate.arg( Utils::json2String( cfg.preferences.ankiConnectServer.deck ),
                                       Utils::json2String( cfg.preferences.ankiConnectServer.model ),
                                       Utils::json2String( fields ),
                                       Utils::json2String( audioArray ),
                                       Utils::json2String( options ) );

  //  qDebug().noquote() << postData;
  postToAnki( postData, autoSend );
}

void AnkiConnector::ankiSearch( const QString & word )
{
  if ( !cfg.preferences.ankiConnectServer.enabled ) {
    emit this->errorText( tr( "Anki search: AnkiConnect is not enabled." ) );
    return;
  }

  QString postTemplate = R"anki({
        "action": "guiBrowse",
        "version": 6,
        "params": {
            "query": "%1"
        }
    })anki";
  postToAnki( postTemplate.arg( word ) );
}

void AnkiConnector::postToAnki( const QString & postData, bool autoSend )
{
  QUrl url;
  url.setScheme( "http" );
  url.setHost( cfg.preferences.ankiConnectServer.host );
  url.setPort( cfg.preferences.ankiConnectServer.port );
  QNetworkRequest request( url );
  request.setTransferTimeout( transfer_timeout );
  //  request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy );
  request.setHeader( QNetworkRequest::ContentTypeHeader, "application/json" );
  request.setAttribute( QNetworkRequest::User, autoSend );
  if ( autoSend ) {
    request.setAttribute( postDataAttribute, postData );
  }
  auto reply = mgr->post( request, postData.toUtf8() );
  connect( reply, &QNetworkReply::errorOccurred, this, [ this, autoSend ]( QNetworkReply::NetworkError e ) {
    qWarning() << e;
    if ( !autoSend ) { // auto-sent notes are queued by finishedSlot
      emit this->errorText( tr( "Anki: post to Anki failed" ) );
    }
  } );
}

void AnkiConnector::finishedSlot( QNetworkReply * reply )
{
  const QString postData = reply->request().attribute( postDataAttribute ).toString();
  if ( reply->error() == QNetworkReply::NoError ) {
    // Anki is running: forget the queued note, whatever the reply (added, duplicate, wrong deck),
    // and send the queue after a new note
    if ( queue.removeOne( postData ) ) {
      saveQueue();
    }
    else if ( !queue.isEmpty() ) {
      flushQueue();
    }

    const QByteArray bytes   = reply->readAll();
    const QJsonDocument json = QJsonDocument::fromJson( bytes );
    const auto obj           = json.object();

    // Normally AnkiConnect always returns result and error,
    // unless Anki is not running.
    if ( obj.size() == 2 && obj.contains( "result" ) && obj.contains( "error" ) && obj[ "error" ].isNull() ) {
      emit errorText( tr( "Anki: post to Anki success" ) );
    }
    else if ( reply->request().attribute( QNetworkRequest::User ).toBool()
              && obj[ "error" ].toString().contains( "duplicate" ) ) {
      // Auto-sent word is already in the deck: expected, stay quiet.
    }
    else {
      emit errorText( tr( "Anki: post to Anki failed" ) );
    }

    qDebug().noquote() << "Anki response:" << Utils::json2String( obj );
  }
  else if ( !postData.isEmpty() ) {
    qDebug() << "Anki connect error, queued" << reply->errorString();
    if ( !queue.contains( postData ) ) {
      queue.append( postData );
      saveQueue();
      scheduleRetry();
      emit errorText( tr( "Anki is not running: the word will be added when it is" ) );
    }
  }
  else {
    qDebug() << "Anki connect error" << reply->errorString();
    emit errorText( "Anki:" + reply->errorString() );
  }

  reply->deleteLater();
}
