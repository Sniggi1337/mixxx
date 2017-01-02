#include "library/crate/cratestorage.h"

#include "library/crate/crateschema.h"
#include "library/dao/trackschema.h"

#include "util/db/dbconnection.h"
#include "util/db/sqltransaction.h"
#include "util/db/fwdsqlquery.h"

#include <QtDebug>


namespace {

const QString CRATETABLE_NAME = "name";
const QString CRATETABLE_LOCKED = "locked";

const QString CRATE_SUMMARY_VIEW = "crate_summary";

const QString CRATESUMMARY_TRACK_COUNT = "track_count";
const QString CRATESUMMARY_TRACK_DURATION = "track_duration";

const QString kCrateTracksJoin = QString(
        "LEFT JOIN %3 ON %3.%4=%1.%2").arg(
                CRATE_TABLE,
                CRATETABLE_ID,
                CRATE_TRACKS_TABLE,
                CRATETRACKSTABLE_CRATEID);

const QString kLibraryTracksJoin = kCrateTracksJoin + QString(
        " LEFT JOIN %3 ON %3.%4=%1.%2").arg(
                CRATE_TRACKS_TABLE,
                CRATETRACKSTABLE_TRACKID,
                LIBRARY_TABLE,
                LIBRARYTABLE_ID);

const QString kCrateSummaryViewSelect = QString(
        "SELECT %1.*,"
            "COUNT(CASE %2.%4 WHEN 0 THEN 1 ELSE NULL END) AS %5,"
            "SUM(CASE %2.%4 WHEN 0 THEN %2.%3 ELSE 0 END) AS %6 "
            "FROM %1").arg(
                CRATE_TABLE,
                LIBRARY_TABLE,
                LIBRARYTABLE_DURATION,
                LIBRARYTABLE_MIXXXDELETED,
                CRATESUMMARY_TRACK_COUNT,
                CRATESUMMARY_TRACK_DURATION);

const QString kCrateSummaryViewQuery = QString(
            "CREATE TEMPORARY VIEW IF NOT EXISTS %1 AS %2 %3 GROUP BY %4.%5").arg(
                    CRATE_SUMMARY_VIEW,
                    kCrateSummaryViewSelect,
                    kLibraryTracksJoin,
                    CRATE_TABLE,
                    CRATETABLE_ID);


class CrateQueryBinder {
  public:
    explicit CrateQueryBinder(FwdSqlQuery& query)
        : m_query(query) {
    }
    virtual ~CrateQueryBinder() = default;

    void bindId(const QString& placeholder, const Crate& crate) const {
        m_query.bindValue(placeholder, crate.getId());
    }
    void bindName(const QString& placeholder, const Crate& crate) const {
        m_query.bindValue(placeholder, crate.getName());
    }
    void bindLocked(const QString& placeholder, const Crate& crate) const {
        m_query.bindValue(placeholder, crate.isLocked());
    }
    void bindAutoDjSource(const QString& placeholder, const Crate& crate) const {
        m_query.bindValue(placeholder, crate.isAutoDjSource());
    }

  protected:
    FwdSqlQuery& m_query;
};


class CrateSummaryQueryBinder: public CrateQueryBinder {
  public:
    explicit CrateSummaryQueryBinder(FwdSqlQuery& query)
        : CrateQueryBinder(query) {
    }
    ~CrateSummaryQueryBinder() override = default;

    void bindTrackCount(const QString& placeholder, const CrateSummary& crateSummary) const {
        m_query.bindValue(placeholder, crateSummary.getTrackCount());
    }
    void bindTrackDuration(const QString& placeholder, const CrateSummary& crateSummary) const {
        m_query.bindValue(placeholder, crateSummary.getTrackDuration());
    }
};

} // anonymous namespace


CrateQueryFields::CrateQueryFields(const FwdSqlQuery& query)
    : m_iId(query.fieldIndex(CRATETABLE_ID)),
      m_iName(query.fieldIndex(CRATETABLE_NAME)),
      m_iLocked(query.fieldIndex(CRATETABLE_LOCKED)),
      m_iAutoDjSource(query.fieldIndex(CRATETABLE_AUTODJ_SOURCE)) {
}


void CrateQueryFields::readValues(const FwdSqlQuery& query, Crate* pCrate) const {
    pCrate->setId(getId(query));
    pCrate->setName(getName(query));
    pCrate->setLocked(isLocked(query));
    pCrate->setAutoDjSource(isAutoDjSource(query));
}


CrateTrackQueryFields::CrateTrackQueryFields(const FwdSqlQuery& query)
    : m_iCrateId(query.fieldIndex(CRATETRACKSTABLE_CRATEID)),
      m_iTrackId(query.fieldIndex(CRATETRACKSTABLE_TRACKID)) {
}


CrateSummaryQueryFields::CrateSummaryQueryFields(const FwdSqlQuery& query)
    : CrateQueryFields(query),
      m_iTrackCount(query.fieldIndex(CRATESUMMARY_TRACK_COUNT)),
      m_iTrackDuration(query.fieldIndex(CRATESUMMARY_TRACK_DURATION)) {
}

void CrateSummaryQueryFields::readValues(
        const FwdSqlQuery& query,
        CrateSummary* pCrateSummary) const {
    CrateQueryFields::readValues(query, pCrateSummary);
    pCrateSummary->setTrackCount(getTrackCount(query));
    pCrateSummary->setTrackDuration(getTrackDuration(query));
}


void CrateStorage::repairDatabase(QSqlDatabase database) {
    DEBUG_ASSERT(!m_database.isOpen());

    // Crates
    {
        FwdSqlQuery query(database, QString(
                "DELETE FROM %1 WHERE %2 IS NULL OR TRIM(%2)=''").arg(
                        CRATE_TABLE,
                        CRATETABLE_NAME));
        if (query.execPrepared() && (query.numRowsAffected() > 0)) {
            qWarning() << "Deleted" << query.numRowsAffected() << "crates with empty names";
        }
    }
    {
        FwdSqlQuery query(database, QString(
                "UPDATE %1 SET %2=0 WHERE %2 NOT IN (0,1)").arg(
                        CRATE_TABLE,
                        CRATETABLE_LOCKED));
        if (query.execPrepared() && (query.numRowsAffected() > 0)) {
            qWarning() << "Fixed boolean values in"
                    "table" << CRATE_TABLE
                    << "column" << CRATETABLE_LOCKED
                    << "for" << query.numRowsAffected() << "crates";
        }
    }
    {
        FwdSqlQuery query(database, QString(
                "UPDATE %1 SET %2=0 WHERE %2 NOT IN (0,1)").arg(
                        CRATE_TABLE,
                        CRATETABLE_AUTODJ_SOURCE));
        if (query.execPrepared() && (query.numRowsAffected() > 0)) {
            qWarning() << "Fixed boolean values in"
                    "table" << CRATE_TABLE
                    << "column" << CRATETABLE_AUTODJ_SOURCE
                    << "for" << query.numRowsAffected() << "crates";
        }
    }

    // Crate tracks
    {
        FwdSqlQuery query(database, QString(
                "DELETE FROM %1 WHERE %2 NOT IN (SELECT %3 FROM %4)").arg(
                        CRATE_TRACKS_TABLE,
                        CRATETRACKSTABLE_CRATEID,
                        CRATETABLE_ID,
                        CRATE_TABLE));
        if (query.execPrepared() && (query.numRowsAffected() > 0)) {
            qWarning() << "Deleted" << query.numRowsAffected() << "crate tracks of non-existent crates";
        }
    }
    {
        FwdSqlQuery query(database, QString(
                "DELETE FROM %1 WHERE %2 NOT IN (SELECT %3 FROM %4)").arg(
                        CRATE_TRACKS_TABLE,
                        CRATETRACKSTABLE_TRACKID,
                        LIBRARYTABLE_ID,
                        LIBRARY_TABLE));
        if (query.execPrepared() && (query.numRowsAffected() > 0)) {
            qWarning() << "Deleted" << query.numRowsAffected() << "crate tracks of non-existent tracks";
        }
    }
}


void CrateStorage::attachDatabase(QSqlDatabase database) {
    m_database = database;
    createViews();
}


void CrateStorage::detachDatabase() {
}


void CrateStorage::createViews() {
    FwdSqlQuery(m_database, kCrateSummaryViewQuery).execPrepared();
}


uint CrateStorage::countCrates() const {
    FwdSqlQuery query(m_database, QString(
            "SELECT COUNT(*) FROM %1").arg(
                    CRATE_TABLE));
    if (query.execPrepared() && query.next()) {
        uint result = query.fieldValue(0).toUInt();
        DEBUG_ASSERT(!query.next());
        return result;
    } else {
        return 0;
    }
}


bool CrateStorage::readCrateById(CrateId id, Crate* pCrate) const {
    FwdSqlQuery query(m_database, QString(
            "SELECT * FROM %1 WHERE %2=:id").arg(
                    CRATE_TABLE,
                    CRATETABLE_ID));
    query.bindValue(":id", id);
    if (query.execPrepared()) {
        CrateSelectIterator crates(query);
        if ((pCrate != nullptr) ? crates.readNext(pCrate) : crates.next()) {
            DEBUG_ASSERT_AND_HANDLE(!crates.next()) {
                qWarning() << "Ambiguous crate id:" << id;
            }
            return true;
        } else {
            qWarning() << "Crate not found by id:" << id;
        }
    }
    return false;
}


bool CrateStorage::readCrateByName(const QString& name, Crate* pCrate) const {
    FwdSqlQuery query(m_database, QString(
            "SELECT * FROM %1 WHERE %2=:name").arg(
                    CRATE_TABLE,
                    CRATETABLE_NAME));
    query.bindValue(":name", name);
    if (query.execPrepared()) {
        CrateSelectIterator crates(query);
        if ((pCrate != nullptr) ? crates.readNext(pCrate) : crates.next()) {
            DEBUG_ASSERT_AND_HANDLE(!crates.next()) {
                qWarning() << "Ambiguous crate name:" << name;
            }
            return true;
        } else {
            qDebug() << "Crate not found by name:" << name;
        }
    }
    return false;
}


CrateSelectIterator CrateStorage::selectCrates() const {
    FwdSqlQuery query(m_database, QString(
            "SELECT * FROM %1 ORDER BY %2 COLLATE %3").arg(
            CRATE_TABLE,
            CRATETABLE_NAME,
            DbConnection::kStringCollationFunc));

    if (query.execPrepared()) {
        return CrateSelectIterator(query);
    } else {
        return CrateSelectIterator();
    }
}


CrateSelectIterator CrateStorage::selectCratesByIds(
        const QString& subselectForCrateIds,
        SqlSubselectMode subselectMode) const {
    QString subselectPrefix;
    switch (subselectMode) {
    case SQL_SUBSELECT_IN:
        if (subselectForCrateIds.isEmpty()) {
            // edge case: no crates
            return CrateSelectIterator();
        }
        subselectPrefix = "IN";
        break;
    case SQL_SUBSELECT_NOT_IN:
        if (subselectForCrateIds.isEmpty()) {
            // edge case: all crates
            return selectCrates();
        }
        subselectPrefix = "NOT IN";
        break;
    }
    DEBUG_ASSERT(!subselectPrefix.isEmpty());
    DEBUG_ASSERT(!subselectForCrateIds.isEmpty());

    FwdSqlQuery query(m_database, QString(
            "SELECT * FROM %1 WHERE %2 %3 (%4) ORDER BY %5 COLLATE %6").arg(
            CRATE_TABLE,
            CRATETABLE_ID,
            subselectPrefix,
            subselectForCrateIds,
            CRATETABLE_NAME,
            DbConnection::kStringCollationFunc));

    if (query.execPrepared()) {
        return CrateSelectIterator(query);
    } else {
        return CrateSelectIterator();
    }
}


CrateSelectIterator CrateStorage::selectAutoDjCrates(bool autoDjSource) const {
    FwdSqlQuery query(m_database, QString(
            "SELECT * FROM %1 WHERE %2=:autoDjSource ORDER BY %3 COLLATE %4").arg(
            CRATE_TABLE,
            CRATETABLE_AUTODJ_SOURCE,
            CRATETABLE_NAME,
            DbConnection::kStringCollationFunc));
    query.bindValue(":autoDjSource", autoDjSource);
    if (query.execPrepared()) {
        return CrateSelectIterator(query);
    } else {
        return CrateSelectIterator();
    }
}


CrateSummarySelectIterator CrateStorage::selectCrateSummaries() const {
    FwdSqlQuery query(m_database, QString(
            "SELECT * FROM %1 ORDER BY %2 COLLATE %3").arg(
            CRATE_SUMMARY_VIEW,
            CRATETABLE_NAME,
            DbConnection::kStringCollationFunc));
    if (query.execPrepared()) {
        return CrateSummarySelectIterator(query);
    } else {
        return CrateSummarySelectIterator();
    }
}

bool CrateStorage::readCrateSummaryById(CrateId id, CrateSummary* pCrateSummary) const {
    FwdSqlQuery query(m_database, QString(
            "SELECT * FROM %1 WHERE %2=:id").arg(
                    CRATE_SUMMARY_VIEW,
                    CRATETABLE_ID));
    query.bindValue(":id", id);
    if (query.execPrepared()) {
        CrateSummarySelectIterator crateSummaries(query);
        if ((pCrateSummary != nullptr) ? crateSummaries.readNext(pCrateSummary) : crateSummaries.next()) {
            DEBUG_ASSERT_AND_HANDLE(!crateSummaries.next()) {
                qWarning() << "Ambiguous crate id:" << id;
            }
            return true;
        } else {
            qWarning() << "Crate summary not found by id:" << id;
        }
    }
    return false;
}


uint CrateStorage::countCrateTracks(CrateId crateId) const {
    FwdSqlQuery query(m_database, QString(
            "SELECT COUNT(*) FROM %1 WHERE %2=:crateId").arg(
                    CRATE_TRACKS_TABLE,
                    CRATETRACKSTABLE_CRATEID));
    query.bindValue(":crateId", crateId);
    if (query.execPrepared() && query.next()) {
        uint result = query.fieldValue(0).toUInt();
        DEBUG_ASSERT(!query.next());
        return result;
    } else {
        return 0;
    }
}


//static
QString CrateStorage::formatSubselectQueryForCrateTrackIds(
        CrateId crateId) {
    return QString("SELECT %1 FROM %2 WHERE %3=%4").arg(
            CRATETRACKSTABLE_TRACKID,
            CRATE_TRACKS_TABLE,
            CRATETRACKSTABLE_CRATEID,
            crateId.toString());
}


CrateTrackSelectIterator CrateStorage::selectCrateTracksSorted(CrateId crateId) const {
    FwdSqlQuery query(m_database, QString(
            "SELECT * FROM %1 WHERE %2=:crateId ORDER BY %3").arg(
                    CRATE_TRACKS_TABLE,
                    CRATETRACKSTABLE_CRATEID,
                    CRATETRACKSTABLE_TRACKID));
    query.bindValue(":crateId", crateId);
    if (query.execPrepared()) {
        return CrateTrackSelectIterator(query);
    } else {
        return CrateTrackSelectIterator();
    }
}


CrateTrackSelectIterator CrateStorage::selectTrackCratesSorted(TrackId trackId) const {
    FwdSqlQuery query(m_database, QString(
            "SELECT * FROM %1 WHERE %2=:trackId ORDER BY %3").arg(
                    CRATE_TRACKS_TABLE,
                    CRATETRACKSTABLE_TRACKID,
                    CRATETRACKSTABLE_CRATEID));
    query.bindValue(":trackId", trackId);
    if (query.execPrepared()) {
        return CrateTrackSelectIterator(query);
    } else {
        return CrateTrackSelectIterator();
    }
}


QSet<CrateId> CrateStorage::collectCrateIdsOfTracks(const QList<TrackId>& trackIds) const {
    // NOTE(uklotzde): One query per track id. This could be optimized
    // by querying for chunks of track ids and collecting the results.
    QSet<CrateId> trackCrates;
    for (const auto& trackId: trackIds) {
        CrateTrackSelectIterator iter(selectTrackCratesSorted(trackId));
        while (iter.next()) {
            DEBUG_ASSERT(iter.trackId() == trackId);
            trackCrates.insert(iter.crateId());
        }
    }
    return trackCrates;
}


bool CrateStorage::onInsertingCrate(
        SqlTransaction& transaction,
        const Crate& crate,
        CrateId* pCrateId) {
    DEBUG_ASSERT(transaction);
    DEBUG_ASSERT_AND_HANDLE(!crate.getId().isValid()) {
        qWarning() << "Cannot insert crate with a valid id:" << crate.getId();
        return false;
    }
    FwdSqlQuery query(m_database, QString(
            "INSERT INTO %1 (%2,%3,%4) VALUES (:name,:locked,:autoDjSource)").arg(
                    CRATE_TABLE,
                    CRATETABLE_NAME,
                    CRATETABLE_LOCKED,
                    CRATETABLE_AUTODJ_SOURCE));
    DEBUG_ASSERT_AND_HANDLE(query.isPrepared()) {
        return false;
    }
    CrateQueryBinder queryBinder(query);
    queryBinder.bindName(":name", crate);
    queryBinder.bindLocked(":locked", crate);
    queryBinder.bindAutoDjSource(":autoDjSource", crate);
    DEBUG_ASSERT_AND_HANDLE(query.execPrepared()) {
        return false;
    }
    if (query.numRowsAffected() > 0) {
        DEBUG_ASSERT(query.numRowsAffected() == 1);
        if (pCrateId != nullptr) {
            *pCrateId = CrateId(query.lastInsertId());
            DEBUG_ASSERT(pCrateId->isValid());
        }
        return true;
    } else {
        return false;
    }
}


bool CrateStorage::onUpdatingCrate(
        SqlTransaction& transaction,
        const Crate& crate) {
    DEBUG_ASSERT(transaction);
    DEBUG_ASSERT_AND_HANDLE(crate.getId().isValid()) {
        qWarning() << "Cannot update crate without a valid id";
        return false;
    }
    FwdSqlQuery query(m_database, QString(
            "UPDATE %1 SET %2=:name,%3=:locked,%4=:autoDjSource WHERE %5=:id").arg(
                    CRATE_TABLE,
                    CRATETABLE_NAME,
                    CRATETABLE_LOCKED,
                    CRATETABLE_AUTODJ_SOURCE,
                    CRATETABLE_ID));
    DEBUG_ASSERT_AND_HANDLE(query.isPrepared()) {
        return false;
    }
    CrateQueryBinder queryBinder(query);
    queryBinder.bindId(":id", crate);
    queryBinder.bindName(":name", crate);
    queryBinder.bindLocked(":locked", crate);
    queryBinder.bindAutoDjSource(":autoDjSource", crate);
    DEBUG_ASSERT_AND_HANDLE(query.execPrepared()) {
        return false;
    }
    if (query.numRowsAffected() > 0) {
        DEBUG_ASSERT_AND_HANDLE(query.numRowsAffected() <= 1) {
            qWarning() << "Updated multiple crates with the same id" << crate.getId();
        }
        return true;
    } else {
        qWarning() << "Cannot update non-existent crate with id" << crate.getId();
        return false;
    }
}


bool CrateStorage::onDeletingCrate(
        SqlTransaction& transaction,
        CrateId crateId) {
    DEBUG_ASSERT_AND_HANDLE(transaction) {
        return false;
    }
    DEBUG_ASSERT_AND_HANDLE(crateId.isValid()) {
        qWarning() << "Cannot delete crate without a valid id";
        return false;
    }
    {
        FwdSqlQuery query(m_database, QString(
                "DELETE FROM %1 WHERE %2=:id").arg(
                        CRATE_TRACKS_TABLE,
                        CRATETRACKSTABLE_CRATEID));
        DEBUG_ASSERT_AND_HANDLE(query.isPrepared()) {
            return false;
        }
        query.bindValue(":id", crateId);
        DEBUG_ASSERT_AND_HANDLE(query.execPrepared()) {
            return false;
        }
        if (query.numRowsAffected() <= 0) {
            qDebug() << "Deleting empty crate with id" << crateId;
        }
    }
    {
        FwdSqlQuery query(m_database, QString(
                "DELETE FROM %1 WHERE %2=:id").arg(
                        CRATE_TABLE,
                        CRATETABLE_ID));
        DEBUG_ASSERT_AND_HANDLE(query.isPrepared()) {
            return false;
        }
        query.bindValue(":id", crateId);
        DEBUG_ASSERT_AND_HANDLE(query.execPrepared()) {
            return false;
        }
        if (query.numRowsAffected() > 0) {
            DEBUG_ASSERT_AND_HANDLE(query.numRowsAffected() <= 1) {
                qWarning() << "Deleted multiple crates with the same id" << crateId;
            }
            return true;
        } else {
            qWarning() << "Cannot delete non-existent crate with id" << crateId;
            return false;
        }
    }
}


bool CrateStorage::onAddingCrateTracks(
        SqlTransaction& transaction,
        CrateId crateId,
        const QList<TrackId>& trackIds) {
    DEBUG_ASSERT_AND_HANDLE(transaction) {
        return false;
    }
    FwdSqlQuery query(m_database, QString(
            "INSERT OR IGNORE INTO %1 (%2, %3) VALUES (:crateId,:trackId)").arg(
                    CRATE_TRACKS_TABLE,
                    CRATETRACKSTABLE_CRATEID,
                    CRATETRACKSTABLE_TRACKID));
    if (!query.isPrepared()) {
        return false;
    }
    query.bindValue(":crateId", crateId);
    for (const auto& trackId: trackIds) {
        query.bindValue(":trackId", trackId);
        if (!query.execPrepared()) {
            return false;
        }
        if (query.numRowsAffected() == 0) {
            // track is already in crate
            qDebug() << "Track" << trackId << "not added to crate" << crateId;
        } else {
            DEBUG_ASSERT(query.numRowsAffected() == 1);
        }
    }
    return true;
}


bool CrateStorage::onRemovingCrateTracks(
        SqlTransaction& transaction,
        CrateId crateId,
        const QList<TrackId>& trackIds) {
    // NOTE(uklotzde): We remove tracks in a transaction and loop
    // analogously to adding tracks (see above).
    DEBUG_ASSERT_AND_HANDLE(transaction) {
        return false;
    }
    FwdSqlQuery query(m_database, QString(
            "DELETE FROM %1 WHERE %2=:crateId AND %3=:trackId").arg(
                    CRATE_TRACKS_TABLE,
                    CRATETRACKSTABLE_CRATEID,
                    CRATETRACKSTABLE_TRACKID));
    if (!query.isPrepared()) {
        return false;
    }
    query.bindValue(":crateId", crateId);
    for (const auto& trackId: trackIds) {
        query.bindValue(":trackId", trackId);
        if (!query.execPrepared()) {
            return false;
        }
        if (query.numRowsAffected() == 0) {
            // track not found in crate
            qDebug() << "Track" << trackId << "not removed from crate" << crateId;
        } else {
            DEBUG_ASSERT(query.numRowsAffected() == 1);
        }
    }
    return true;
}


bool CrateStorage::onPurgingTracks(
        SqlTransaction& transaction,
        const QList<TrackId>& trackIds) {
    DEBUG_ASSERT(transaction);

    // NOTE(uklotzde): Remove tracks from crates one-by-one.
    // This might be optimized by deleting multiple track ids
    // at once in chunks with a maximum size.
    FwdSqlQuery query(m_database, QString(
            "DELETE FROM %1 WHERE %2=:trackId").arg(
                    CRATE_TRACKS_TABLE,
                    CRATETRACKSTABLE_TRACKID));
    if (!query.isPrepared()) {
        return false;
    }
    for (const auto& trackId: trackIds) {
        query.bindValue(":trackId", trackId);
        if (!query.execPrepared()) {
            return false;
        }
    }
    return true;
}
