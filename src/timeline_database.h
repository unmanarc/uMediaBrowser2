#pragma once

/** @file timeline_database.h
 *  @brief Read-only digiKam access for the timeline HTTP services.
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
#include "timeline_filter.h"

namespace Mantids30::Runtime::Logs { class AppLog; }

/** @brief Provides timeline pages directly from digiKam through libMantids30.
 *
 * Each operation opens the nearest ancestor digikam4.db in SQLite mode=ro and
 * holds a read transaction for that request. SQL orders and filters the rows;
 * the application retains only the requested page and at most six samples per
 * returned event. A separate SQLite memory snapshot stores existing paths for
 * the tag picker, built at startup and replaced only by an explicit rescan.
 *
 * Existing-file counts and configured event boundaries require streaming the
 * matching rows and checking their files. This uses bounded application memory,
 * rather than copying all Images records into a searchable container. SQLite
 * may use its own temporary storage to execute ORDER BY. Dates use a valid IMG
 * filename first, ImageInformation.creationDate second, and finally
 * Images.modificationDate. The original modification time and selected source
 * are exposed separately in Media.
 *
 * Validated TimelineFilter conditions run inside SQLite before grouping, counts,
 * sampling and pagination. Date filters always use ImageInformation.creationDate,
 * independently of the selected grouping date. Filter identity scopes revisions,
 * so every event/gallery/sample request must repeat the same JSON conditions.
 *
 * @note Instances can be shared by HTTP workers: connections and query state
 *       belong to individual requests. Path snapshots are immutable and shared.
 */
class TimelineDatabase
{
public:
    /** @brief Failure categories that the HTTP adapter maps to response codes. */
    enum class ErrorKind { Unavailable, InvalidArgument, NotFound, Stale };

    /** @brief Database/query failure with an HTTP-independent category. */
    class Error : public std::runtime_error
    {
    public:
        /** @param kind Failure category. @param message Human-readable reason. */
        Error(ErrorKind kind, const std::string &message) : std::runtime_error(message), kind(kind) {}
        ErrorKind kind; ///< Category consumed by the HTTP adapter.
    };

    /** @brief Public digiKam tag; ancestors supply its descriptive path. */
    struct Tag
    {
        int64_t id = 0;
        int64_t parent = 0;
        std::string name; ///< Exact name used by the existing tag filter contract.
        std::string path; ///< Ancestor names for distinguishing tag branches.
        size_t count = 0; ///< Distinct assigned files (tag catalog only).
    };

    /** @brief One existing file; paths are relative to the configured HTTP root. */
    struct Media
    {
        std::string path;   ///< URI-style path, beginning with '/'.
        std::string folder; ///< Parent URI-style path; '/' denotes rootdir.
        int64_t timestamp = 0; ///< First valid date: IMG filename, creationDate, modificationDate.
        int64_t mtime = 0; ///< Original Images.modificationDate as Unix seconds; 0 if invalid.
        std::string dateSource; ///< "filename", "creationDate" or "modificationDate".
        uintmax_t size = 0;    ///< Images.fileSize; no filesystem metadata extraction.
        size_t index = 0;      ///< Position in the event's folder/time ordering.
        std::vector<Tag> tags; ///< Assigned public tags, sorted by full path.
    };

    /** @brief A contiguous run of existing files separated by less than gapSeconds(). */
    struct Event
    {
        std::string id; ///< Opaque revision and time bounds, usable for direct range queries.
        int64_t start = 0; ///< Oldest selected capture time, inclusive.
        int64_t end = 0;   ///< Newest selected capture time, inclusive.
        size_t count = 0;  ///< Number of currently existing files in the event.
        std::vector<Media> samples; ///< Up to six random members with their gallery positions.
        std::vector<Tag> tags; ///< Union of assigned public tags across all existing event files.
    };

    /** @brief A bounded event feed and totals calculated by streaming the query. */
    struct FeedPage
    {
        std::string revision; ///< Database file/WAL identity plus the application revision.
        bool databaseAvailable = false; ///< False when the database cannot be read.
        std::string error; ///< Availability error; folders remain usable independently.
        size_t skipped = 0; ///< Missing/unavailable files or invalid dates encountered.
        size_t totalItems = 0; ///< Existing files under rootdir matching the filter.
        size_t totalEvents = 0; ///< Events formed from those files.
        size_t next = 0; ///< Offset for the next feed request.
        bool hasMore = false; ///< Whether more events follow this page.
        std::vector<Event> events; ///< Only the requested events, newest first.
    };

    /** @brief A bounded page of event files, ordered by folder, time and path. */
    struct EventPage
    {
        Event event; ///< Current event bounds and existing-file count.
        std::vector<Media> items; ///< Requested gallery page; never the entire event.
        size_t next = 0; ///< Offset among existing event files.
        bool hasMore = false; ///< Whether additional existing files follow.
    };

    /** @brief Query tags through SQL joins, without filesystem existence checks. */
    std::vector<Tag> tags() const;

    /** @brief Rebuild existing paths, publishing atomically after a successful scan.
     * Existing readers keep the previous snapshot. Errors preserve that snapshot.
     * @return Number of regular files scanned under rootdir (excluding symlinks).
     */
    size_t rescanTagFiles();

    /** @brief Configure database access and scan existing paths for the tag picker.
     * @param root Existing directory passed via --rootdir; canonicalized once.
     * @param gapSeconds Inactivity threshold starting a new event (default: 30 minutes).
     * @param logger Optional application logger; must outlive this instance.
     * @param tagExclusions Compiled expressions hiding matching tags and their descendants from the picker and informational badges.
     * @throws std::filesystem::filesystem_error If root cannot be resolved.
     */
    explicit TimelineDatabase(std::string root, int64_t gapSeconds = 30 * 60,
                              Mantids30::Runtime::Logs::AppLog *logger = nullptr,
                              std::vector<std::regex> tagExclusions = {});

    /** @brief Read a feed page using a request-local database transaction.
     * @param cursor Number of events to skip.
     * @param limit Maximum events to return (1..12).
     * @param revision Previous page's revision; required for nonzero cursor.
     * @param filter Validated SQL conditions applied before event grouping.
     * @return Requested events and totals, or an availability error in the page.
     * @throws Error For invalid pagination or a changed revision.
     */
    FeedPage feed(size_t cursor = 0, size_t limit = 8, const std::string &revision = "", const TimelineFilter &filter = {}) const;

    /** @brief Query an event directly by the time bounds encoded in its ID.
     * @param id Event identifier returned by feed().
     * @param cursor Number of existing event files to skip.
     * @param limit Maximum files to retain (1..60).
     * @param filter Same conditions used by feed(); mismatches invalidate the ID.
     * @return Current event information and the requested file page.
     * @throws Error For an invalid/stale ID, unavailable database or invalid cursor.
     */
    EventPage event(const std::string &id, size_t cursor = 0, size_t limit = 60, const TimelineFilter &filter = {}) const;

    /** @brief Select up to six distinct existing files using reservoir sampling.
     * @param id Event identifier returned by feed().
     * @param filter Same conditions used by feed(), including for sample positions.
     * @return Event information and random files with their gallery positions.
     * @throws Error For an invalid/stale ID or unavailable database.
     */
    Event samples(const std::string &id, const TimelineFilter &filter = {}) const;

    /** @brief Invalidate client cursors after refresh/upload/delete; reads no files. */
    void invalidate() { generation.fetch_add(1); }

    /** @return Configured inactivity threshold for starting a new event. */
    int64_t gapSeconds() const { return gap; }

private:
    class Reader; ///< Owns one read-only connection, transaction and streaming queries.
    struct FileSnapshot; ///< Keeps a named SQLite memory database alive.
    std::string root; ///< Canonical HTTP root used to constrain every query.
    int64_t gap; ///< Event gap in seconds, always positive.
    std::atomic<uint64_t> generation; ///< Revision seed; never contains database rows.
    mutable std::mutex snapshotMutex; ///< Protects snapshot publication only.
    std::mutex scanMutex; ///< Serializes manual rescans.
    std::shared_ptr<FileSnapshot> fileSnapshot; ///< Immutable existing-path table.
    Mantids30::Runtime::Logs::AppLog *logger; ///< Application-owned scan logger.
    const std::vector<std::regex> tagExclusions; ///< Tag visibility only; never media filtering.
};
