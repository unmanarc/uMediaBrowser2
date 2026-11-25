/** @file timeline_database.cpp
 *  @brief Streaming implementation of the read-only timeline database layer.
 */
#include "timeline_database.h"
#include <Mantids30/Runtime/Logs/applog.h>
#include <Mantids30/DB/SQLite3/sqlconnector_sqlite3.h>
#include <Mantids30/Core/Memory/a_int64.h>
#include <Mantids30/Core/Memory/a_string.h>
#include <Mantids30/Core/Memory/a_double.h>
#include <Mantids30/Core/Memory/a_bin.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using Mantids30::DB::SQLite3::SQLConnector_SQLite3;
using Mantids30::DB::Common::Query;
using namespace Mantids30::Core::Memory::Abstract;
using Error = TimelineDatabase::Error;
using ErrorKind = TimelineDatabase::ErrorKind;
using Bindings = std::map<std::string, std::shared_ptr<Var>>;

namespace {
/** @brief Test containment by path components, avoiding same-prefix siblings. */
bool inside(const fs::path &path, const fs::path &root)
{
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p)
        if (p == path.end() || *p != *r) return false;
    return true;
}

/** @brief Normalize directory paths, including root-album trailing slashes. */
fs::path normalized(fs::path path)
{
    path = path.lexically_normal();
    if (path.has_relative_path() && path.filename().empty()) path = path.parent_path();
    return path;
}

/** @brief Escape a file URI so path characters cannot become SQLite options. */
std::string uriEncode(const std::string &value)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value)
    {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') result += c;
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

/** @brief Decode a digiKam collection URL while preserving literal plus signs. */
std::string uriDecode(const std::string &value)
{
    std::string result;
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '%' && i + 2 < value.size() && std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[i + 2])))
        {
            result += static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        else result += value[i]; // '+' is a literal path character in a URL query.
    }
    return result;
}

/** @brief Resolve digiKam path= identifiers or absolute specificPath values. */
fs::path collectionPath(const std::string &identifier, const std::string &specific)
{
    const auto question = identifier.find('?');
    if (question != std::string::npos)
    {
        size_t begin = question + 1;
        while (begin < identifier.size())
        {
            const auto end = identifier.find('&', begin);
            const auto part = identifier.substr(begin, end == std::string::npos ? end : end - begin);
            if (part.compare(0, 5, "path=") == 0)
            {
                fs::path volume = uriDecode(part.substr(5));
                if (!volume.is_absolute()) return {};
                return normalized(volume / fs::path(specific).relative_path());
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    // Collections on the system volume can store their absolute path here.
    fs::path path(specific);
    return path.is_absolute() ? normalized(path) : fs::path{};
}

/** @brief Find the nearest digikam4.db, inspecting rootdir and every parent to /. */
fs::path findDatabase(fs::path directory)
{
    for (;;)
    {
        const auto candidate = directory / "digikam4.db";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
        const auto parent = directory.parent_path();
        if (parent == directory) return {};
        directory = parent;
    }
}

/** @brief Describe the database and WAL without modifying either file.
 * Includes inode and nanosecond change times to detect replacement, commits,
 * and WAL writes even when the database size does not change.
 */
std::string fingerprint(const fs::path &database)
{
    std::string result = database.string();
    for (const std::string &suffix : {std::string{}, std::string{"-wal"}})
    {
        struct stat status{};
        if (::stat((database.string() + suffix).c_str(), &status) != 0)
        {
            if (suffix.empty()) throw Error(ErrorKind::Unavailable, "Cannot read digiKam database file.");
            result += ":no-wal";
            continue;
        }
        // Opening a WAL database can create an empty sidecar even for a
        // read-only connection. No WAL and a zero-byte WAL both contain no
        // committed frames and must therefore have the same revision identity.
        if (!suffix.empty() && status.st_size == 0)
        {
            result += ":no-wal";
            continue;
        }
        for (const auto value : {static_cast<int64_t>(status.st_dev), static_cast<int64_t>(status.st_ino),
                                 static_cast<int64_t>(status.st_size), static_cast<int64_t>(status.st_mtim.tv_sec),
                                 static_cast<int64_t>(status.st_mtim.tv_nsec), static_cast<int64_t>(status.st_ctim.tv_sec),
                                 static_cast<int64_t>(status.st_ctim.tv_nsec)})
            result += ":" + std::to_string(value);
    }
    return result;
}

/** @brief Reject failed preparations and interrupted/failed result iteration. */
void checkQuery(const std::shared_ptr<Query> &query, bool finished = false)
{
    if (!query || !query->isSuccessful() || (finished && query->getLastSQLReturnValue() != SQLITE_DONE))
        throw Error(ErrorKind::Unavailable, "Cannot read digiKam database: " +
                    (query ? query->getLastSQLError() : "query unavailable"));
}

/** @brief Infer relocations by shared directory names, accepting only a unique
 * candidate with an indexed regular file beneath rootdir. Work is bounded to
 * 64 file probes per candidate; ambiguous or unverified paths stay unchanged.
 */
std::map<std::string, std::string> relocatedCollections(const std::string &root,
                                                      Mantids30::Runtime::Logs::AppLog *logger)
{
    using namespace Mantids30::Runtime::Logs;
    std::map<std::string, std::string> result;
    const auto database = findDatabase(root);
    if (logger) logger->log0("timeline", INFO, "Collection path detection: root=%s database=%s",
                            root.c_str(), database.empty() ? "not found" : database.c_str());
    if (database.empty()) return result;
    SQLConnector_SQLite3 connection;
    connection.setThrowCPPErrorOnQueryFailure(false);
    if (!sqlite3_compileoption_used("USE_URI") ||
        !connection.connect("file:" + uriEncode(database.string()) + "?mode=ro") || !connection.qExecuteEx("BEGIN"))
        throw Error(ErrorKind::Unavailable, "Cannot open digiKam database for collection path detection.");
    std::set<std::string> nativePaths;
    std::vector<std::pair<std::string, std::string>> storedRoots;
    {
        STRING identifier, specific;
        auto roots = connection.qSelect("SELECT identifier,specificPath FROM AlbumRoots", {}, {&identifier, &specific});
        checkQuery(roots);
        while (roots->step())
            if (!roots->isNull(0) && !roots->isNull(1))
                storedRoots.emplace_back(identifier.getValue(), specific.getValue());
        checkQuery(roots, true);
    } // Release the connector's active query before probing files.
    for (const auto &stored : storedRoots)
    {
        const auto original = collectionPath(stored.first, stored.second);
        if (original.empty()) continue;
        if (inside(root, original) || inside(original, root))
        {
            nativePaths.insert(original.string());
            continue;
        }
        std::set<fs::path> candidates, verified;
        // Align equal path components, keeping the collection suffix intact.
        // This covers both a served collection and a served subtree of it.
        for (auto oldAnchor = original; oldAnchor != oldAnchor.root_path(); oldAnchor = oldAnchor.parent_path())
            for (auto newAnchor = fs::path(root); newAnchor != newAnchor.root_path(); newAnchor = newAnchor.parent_path())
                if (oldAnchor.filename() == newAnchor.filename())
                {
                    const auto suffix = original.lexically_relative(oldAnchor);
                    const auto candidate = normalized(newAnchor / suffix);
                    if (inside(root, candidate) || inside(candidate, root)) candidates.insert(candidate);
                }
        for (const auto &candidate : candidates)
        {
            STRING folder, name;
            auto files = connection.qSelect(R"SQL(
                WITH folders AS (
                    SELECT id, CASE WHEN trim(relativePath,'/')='' THEN :collection
                        ELSE rtrim(:collection,'/') || '/' || trim(relativePath,'/') END AS folder
                    FROM Albums WHERE albumRoot IN (
                        SELECT id FROM AlbumRoots WHERE identifier=:identifier AND specificPath=:specific)
                    AND relativePath IS NOT NULL
                    AND instr('/' || trim(relativePath,'/') || '/', '/../')=0
                    AND instr('/' || trim(relativePath,'/') || '/', '/./')=0
                )
                SELECT a.folder,i.name FROM folders a JOIN Images i ON i.album=a.id
                WHERE (a.folder=:root OR substr(a.folder,1,length(:prefix))=:prefix)
                    AND i.name IS NOT NULL AND i.name NOT IN ('','.','..')
                    AND instr(i.name,'/')=0 AND instr(i.name,char(0))=0
                LIMIT 64
            )SQL", {{":collection", std::make_shared<STRING>(candidate.string())},
                     {":identifier", std::make_shared<STRING>(stored.first)},
                     {":specific", std::make_shared<STRING>(stored.second)},
                     {":root", std::make_shared<STRING>(root)},
                     {":prefix", std::make_shared<STRING>(root == "/" ? "/" : root + "/")}}, {&folder, &name});
            checkQuery(files);
            while (files->step())
            {
                std::error_code ec;
                const fs::path directory(folder.getValue());
                if (fs::canonical(directory, ec) != directory || ec) continue;
                if (fs::is_regular_file(fs::symlink_status(directory / name.getValue(), ec)) && !ec)
                    verified.insert(candidate);
            }
            checkQuery(files, true);
        }
        if (verified.size() == 1) result[original.string()] = verified.begin()->string();
        else if (logger) logger->log0("timeline", WARNING,
            "Collection path unresolved: stored=%s root=%s candidates=%zu verified=%zu (need one unique match)",
            original.c_str(), root.c_str(), candidates.size(), verified.size());
    }
    // Never merge two unrelated stored collections into the same local path.
    std::map<std::string, size_t> targets;
    for (const auto &path : nativePaths) ++targets[path];
    for (const auto &entry : result) ++targets[entry.second];
    for (auto entry = result.begin(); entry != result.end();)
    {
        if (targets[entry->second] > 1)
        {
            if (logger) logger->log0("timeline", WARNING, "Ambiguous collection relocation rejected: stored=%s local=%s",
                                    entry->first.c_str(), entry->second.c_str());
            entry = result.erase(entry);
        }
        else
        {
            if (logger) logger->log0("timeline", INFO, "Collection path adapted in memory: stored=%s local=%s",
                                    entry->first.c_str(), entry->second.c_str());
            ++entry;
        }
    }
    return result;
}

/** @brief Build SQLite's equivalent of the observed IMG filename regexes.
 *
 * Regexes (case-insensitive, anchored at the beginning):
 * - IMG[_-]([0-9]{8})[-_]([0-9]{6})($|[^0-9])
 * - IMG[_-]([0-9]{8})-WA[0-9]
 * - IMG[_-]([0-9]{8})($|[.])
 *
 * GLOB digit classes plus fixed offsets implement these patterns inside SQLite,
 * which has no built-in REGEXP. This keeps ORDER BY and event range filtering
 * on the selected date without an extension or an application-side media index.
 * Camera counters such as IMG_1234 and unseparated numeric IDs do not match.
 * Date-only names, including WhatsApp, use midnight in server local time.
 * Calendar/time validity is checked separately by isoTimestampSQL().
 * @return SQL expression producing a local ISO timestamp or NULL.
 */
std::string filenameDateSQL()
{
    const std::string date = "substr(i.name,5,4) || '-' || substr(i.name,9,2) || '-' || substr(i.name,11,2)";
    return "CASE WHEN upper(substr(i.name,1,3))='IMG' AND substr(i.name,4,1) IN ('_','-') "
           "AND substr(i.name,5,8) GLOB '[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]' THEN CASE "
           "WHEN substr(i.name,13,1) IN ('_','-') "
           "AND substr(i.name,14,6) GLOB '[0-9][0-9][0-9][0-9][0-9][0-9]' "
           "AND (length(i.name)=19 OR substr(i.name,20,1) NOT GLOB '[0-9]') THEN " + date +
           " || 'T' || substr(i.name,14,2) || ':' || substr(i.name,16,2) || ':' || substr(i.name,18,2) "
           "WHEN (upper(substr(i.name,13,3))='-WA' AND substr(i.name,16,1) GLOB '[0-9]') "
           "OR length(i.name)=12 OR substr(i.name,13,1)='.' THEN " + date + " || 'T00:00:00' END END";
}

/** @brief Convert a valid ISO date expression to Unix seconds in SQLite.
 * Invalid calendar dates (including non-leap February 29), year zero, invalid
 * times, empty strings and NULL return NULL, allowing COALESCE to try the next
 * source. Dates without an explicit timezone use the server local timezone;
 * Z and numeric offsets are preserved. Fractional seconds are truncated.
 * @param column Internal SQL column name, never supplied by an HTTP client.
 * @return SQL expression yielding signed Unix seconds or NULL.
 */
std::string isoTimestampSQL(const std::string &column)
{
    return "CASE WHEN substr(" + column + ",1,4) BETWEEN '0001' AND '9999' "
           "AND strftime('%Y-%m-%d',substr(" + column + ",1,10),'+0 days')=substr(" + column + ",1,10) "
           "AND (length(" + column + ")<19 OR substr(" + column + ",12,2) BETWEEN '00' AND '23') "
           "THEN CAST(strftime('%s'," + column + ", CASE WHEN length(" + column + ")>=19 "
           "AND instr(substr(" + column + ",20),'Z')=0 AND instr(substr(" + column + ",20),'+')=0 "
           "AND instr(substr(" + column + ",20),'-')=0 THEN 'utc' ELSE '+0 seconds' END) AS INTEGER) END";
}

/** @brief Decode one signed event bound; reject overflow and trailing characters. */
int64_t bound(const std::string &text)
{
    int64_t value;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw Error(ErrorKind::InvalidArgument, "Invalid event");
    return value;
}
}

/** @brief Named memory database kept alive while any tag reader uses it. */
struct TimelineDatabase::FileSnapshot
{
    SQLConnector_SQLite3 connection;
    std::string uri;
};

/** @brief Request-local connection and SQL cursor implementation.
 * No Reader survives its API call. The folder validation map caches only
 * canonical-directory checks during that call, never Images records.
 * Always reopen the discovered path: another process may replace digikam4.db
 * between requests, leaving an existing connection attached to the old file.
 */
class TimelineDatabase::Reader
{
public:
    /** @brief Open a URI-enforced read-only connection and begin a read transaction.
     * @param root Canonical HTTP root.
     * @param version Revision counter captured by the calling API method.
     * @param filter Validated conditions included in SQL scope and revision identity.
     * @param exclusions Rules hiding tag names and their descendants from all tag output.
     * @param overrides Verified startup collection relocations.
     * @param uriPath Optional validated file/folder path narrowing SQL before streaming.
     * @param singleFile Select an exact file instead of a folder subtree.
     * @throws Error If discovery, connection, schema access or URI support fails.
     */
    Reader(const std::string &root, uint64_t version, const TimelineFilter &filter, const std::vector<std::regex> &exclusions,
           const std::map<std::string, std::string> &overrides, const std::string &uriPath = "", bool singleFile = false)
        : overrides(overrides), exclusions(exclusions), root(root), database(findDatabase(root))
    {
        if (database.empty()) throw Error(ErrorKind::Unavailable, "No digikam4.db found in --rootdir or its ancestors.");
        if (!sqlite3_compileoption_used("USE_URI"))
            throw Error(ErrorKind::Unavailable, "SQLite URI support is required for read-only digiKam access.");
        stamp = fingerprint(database);
        revision = std::to_string(std::hash<std::string>{}(stamp + filter.identity())) + "-" + std::to_string(version);
        connection.setThrowCPPErrorOnQueryFailure(false);
        // libMantids30 uses sqlite3_open; USE_URI makes mode=ro an engine-level
        // restriction even if the operating-system user can write this file.
        if (!connection.connect("file:" + uriEncode(database.string()) + "?mode=ro") || !connection.qExecuteEx("BEGIN"))
            throw Error(ErrorKind::Unavailable, "Cannot open digiKam database in read-only mode.");
        prepareScope(filter, uriPath, singleFile);
        // Closing the connection automatically ends this read-only transaction.
    }

    /** @brief Ensure client pagination belongs to this database revision.
     * @param expected Token from the previous page; empty permits an initial feed.
     * @throws Error With ErrorKind::Stale when the token no longer matches.
     */
    void requireRevision(const std::string &expected) const
    {
        if (!expected.empty() && expected != revision)
            throw Error(ErrorKind::Stale, "Timeline changed. Refresh to continue.");
    }

    /** @brief Reject a response if the database/WAL changed while it was read. */
    void verify() const
    {
        if (fingerprint(database) != stamp)
            throw Error(ErrorKind::Stale, "Timeline changed while reading. Refresh to continue.");
    }

    /** @brief Stream rows in SQL order and invoke consume only for existing files.
     * @param range Optional inclusive event time bounds; null means the whole scope.
     * @param byFolder Whether to use gallery ordering instead of feed ordering.
     * @param consume Callback invoked for one temporary Media at a time.
     * @param skipped Receives the number of invalid/missing candidates.
     * @param ascending Stream oldest first when not using gallery ordering.
     * @return Number of SQL candidates, including skipped records.
     */
    size_t walk(const Event *range, bool byFolder, const std::function<void(Media)> &consume, size_t &skipped, bool ascending = false)
    {
        auto input = bindings;
        std::string sql = scope + " SELECT path, folder, timestamp, size, mtime, date_source, ";
        // Only event reads need assignments. Keep the full-library count scan light.
        sql += range ? "(SELECT group_concat(DISTINCT tagid) FROM ImageTags WHERE imageid=media.imageid)" : "NULL";
        sql += " FROM media";
        if (range)
        {
            sql += " WHERE timestamp BETWEEN :start AND :end";
            input[":start"] = std::make_shared<INT64>(range->start);
            input[":end"] = std::make_shared<INT64>(range->end);
        }
        sql += byFolder ? " ORDER BY folder, timestamp DESC, path" : ascending ? " ORDER BY timestamp ASC, path" : " ORDER BY timestamp DESC, path";
        STRING path, folder, dateSource, tagIds;
        INT64 timestamp, size, modified;
        auto query = connection.qSelect(sql, input, {&path, &folder, &timestamp, &size, &modified, &dateSource, &tagIds});
        checkQuery(query);
        size_t candidates = 0;
        while (query->step())
        {
            ++candidates;
            const auto directory = folder.getValue();
            const auto full = path.getValue();
            if (query->isNull(2) || !exists(directory, full)) { ++skipped; continue; }
            Media item;
            item.path = root == "/" ? full : full.substr(root.size());
            item.folder = directory == root ? "/" : root == "/" ? directory : directory.substr(root.size());
            item.timestamp = timestamp.getValue();
            item.mtime = modified.getValue();
            item.dateSource = dateSource.getValue();
            item.size = static_cast<uintmax_t>(std::max<int64_t>(0, size.getValue()));
            if (range && !query->isNull(6))
            {
                const auto ids = tagIds.getValue();
                const char *position = ids.data(), *end = position + ids.size();
                while (position < end)
                {
                    int64_t id;
                    const auto parsed = std::from_chars(position, end, id);
                    if (parsed.ec != std::errc{}) break;
                    auto tag = mediaTags.find(id);
                    if (tag != mediaTags.end()) item.tags.push_back(tag->second);
                    position = parsed.ptr == end ? end : parsed.ptr + 1;
                }
                std::sort(item.tags.begin(), item.tags.end(), tagLess);
            }
            consume(std::move(item));
        }
        checkQuery(query, true);
        return candidates;
    }

    /** @brief Stream time-ordered records, retaining only the requested event page.
     * @param cursor Number of complete events to skip.
     * @param limit Maximum event summaries to retain.
     * @param gap Inactivity threshold starting a new event; equality starts a new group.
     * @param ascending Return oldest events first.
     * @return Requested page with exact existing-file and event totals.
     */
    FeedPage readFeed(size_t cursor, size_t limit, int64_t gap, bool ascending)
    {
        FeedPage page;
        page.revision = revision;
        page.databaseAvailable = true;
        Event current;
        int64_t previous = 0;
        auto finish = [&] {
            if (current.count && page.totalEvents - 1 >= cursor && page.events.size() < limit)
            {
                current.id = revision + ":" + std::to_string(current.start) + ":" + std::to_string(current.end);
                page.events.push_back(std::move(current));
            }
            current = Event{};
        };
        walk(nullptr, false, [&](Media item) {
            if (!current.count || (ascending ? item.timestamp - previous : previous - item.timestamp) >= gap)
            {
                finish();
                ++page.totalEvents;
                current.start = current.end = item.timestamp;
            }
            current.start = std::min(current.start, item.timestamp);
            current.end = std::max(current.end, item.timestamp);
            previous = item.timestamp;
            ++current.count;
            ++page.totalItems;
        }, page.skipped, ascending);
        finish();
        if (cursor > page.totalEvents) throw Error(ErrorKind::InvalidArgument, "Invalid cursor");
        page.next = cursor + page.events.size();
        page.hasMore = page.next < page.totalEvents;
        // Fetch only displayed events by their SQL time ranges. The gallery's
        // folder ordering supplies the sample positions used by the carousel.
        for (auto &event : page.events) event = readEvent(event, 0, 0, true).event;
        return page;
    }

    /** @brief Validate an event ID and extract direct SQL range-query bounds.
     * @param id Revision:start:end token supplied by a previous feed request.
     * @return Bounds for a parameterized SQL query; no event index is searched.
     * @throws Error For malformed bounds or an outdated revision.
     */
    Event decodeEvent(const std::string &id) const
    {
        const auto first = id.find(':');
        const auto second = first == std::string::npos ? first : id.find(':', first + 1);
        if (first == 0 || first == std::string::npos || second == std::string::npos)
            throw Error(ErrorKind::InvalidArgument, "Invalid event");
        Event event;
        event.id = id;
        event.start = bound(id.substr(first + 1, second - first - 1));
        event.end = bound(id.substr(second + 1));
        if (event.start > event.end || event.start < -62167219200LL || event.end > 253402300799LL)
            throw Error(ErrorKind::InvalidArgument, "Invalid event");
        requireRevision(id.substr(0, first));
        return event;
    }

    /** @brief Read one event using bounded page storage or a six-item reservoir.
     * The row cursor counts only existing files. A deleted file therefore leaves
     * no hole in pagination, sample positions or carousel navigation.
     * @param event Revision-validated time bounds to query.
     * @param cursor Number of existing files to skip for an ordinary gallery page.
     * @param limit Maximum gallery files to retain; zero for random samples.
     * @param randomSamples Select a six-file reservoir instead of a gallery page.
     * @return Requested files or samples and the current existing-file count.
     */
    EventPage readEvent(Event event, size_t cursor, size_t limit, bool randomSamples)
    {
        if (!mediaTagsLoaded)
        {
            mediaTags = tagDefinitions();
            mediaTagsLoaded = true;
        }
        std::map<int64_t, Tag> eventTags;
        EventPage page;
        page.event = std::move(event);
        page.event.count = 0;
        size_t skipped = 0;
        thread_local std::mt19937_64 random(std::random_device{}());
        const size_t candidates = walk(&page.event, true, [&](Media item) {
            item.index = page.event.count++;
            for (const auto &tag : item.tags) eventTags.emplace(tag.id, tag);
            if (randomSamples)
            {
                auto &samples = page.event.samples;
                if (samples.size() < 6) samples.push_back(std::move(item));
                else
                {
                    std::uniform_int_distribution<size_t> pick(0, item.index);
                    const size_t position = pick(random);
                    if (position < samples.size()) samples[position] = std::move(item);
                }
            }
            else if (item.index >= cursor && page.items.size() < limit) page.items.push_back(std::move(item));
        }, skipped);
        page.event.tags.clear();
        for (const auto &entry : eventTags) page.event.tags.push_back(entry.second);
        std::sort(page.event.tags.begin(), page.event.tags.end(), tagLess);
        if (!candidates) throw Error(ErrorKind::NotFound, "Event not found");
        if (cursor > page.event.count) throw Error(ErrorKind::InvalidArgument, "Invalid cursor");
        page.next = cursor + page.items.size();
        page.hasMore = page.next < page.event.count;
        if (randomSamples) std::shuffle(page.event.samples.begin(), page.event.samples.end(), random);
        return page;
    }

    std::string revision; ///< Token for the currently open database transaction.

    /** @brief Stream matching existing paths without requiring a timeline date. */
    void walkPaths(const std::function<void(const std::string &)> &consume)
    {
        STRING path, folder;
        auto query = connection.qSelect(scope + " SELECT DISTINCT path, folder FROM image_dates", bindings, {&path, &folder});
        checkQuery(query);
        while (query->step())
        {
            const auto file = path.getValue();
            if (exists(folder.getValue(), file)) consume(root == "/" ? file : file.substr(root.size()));
        }
        checkQuery(query, true);
    }

    /** @brief Read bounded text values through the same path scope as the tag picker. */
    boost::json::object readFilterValues(const std::string &memoryURI)
    {
        checkQuery(connection.qExecute("ATTACH DATABASE :uri AS mem", {{":uri", std::make_shared<STRING>(memoryURI)}}));
        boost::json::object result;
        for (const auto &entry : TimelineFilter::fields())
        {
            const auto &field = entry.as_object();
            if (field.at("type") != "text") continue;
            const std::string name(field.at("field").as_string());
            boost::json::array values;
            // Metadata is optional in older databases; an empty picker is still usable.
            if (connection.dbTableExist("ImageMetadata"))
            {
                const std::string column = "m.\"" + name + "\"";
                STRING value;
                auto query = connection.qSelect(scope + " SELECT DISTINCT " + column + " COLLATE BINARY" +
                    " FROM image_dates i JOIN mem.existing_files f ON f.path=i.path"
                    " JOIN ImageMetadata m ON m.imageid=i.imageid WHERE typeof(" + column + ")='text'"
                    " AND length(CAST(" + column + " AS BLOB)) BETWEEN 1 AND 256"
                    " AND length(trim(" + column + ",char(9,10,11,12,13,32)))>0"
                    " AND instr(" + column + ",char(0))=0 ORDER BY " + column + " COLLATE BINARY",
                    bindings, {&value});
                checkQuery(query);
                while (query->step()) values.emplace_back(value.getValue());
                checkQuery(query, true);
            }
            result[name] = std::move(values);
        }
        return result;
    }

    /** @brief Join the current digiKam rows to an immutable existing-path table.
     * Ancestors provide labels only; a parent without assigned files is not an
     * option. Missing dates do not disqualify a file from the tag catalog.
     */
    std::vector<Tag> readTags(const std::string &memoryURI)
    {
        checkQuery(connection.qExecute("ATTACH DATABASE :uri AS mem", {{":uri", std::make_shared<STRING>(memoryURI)}}));
        const auto definitions = tagDefinitions();
        INT64 id, count;
        std::vector<Tag> result;
        auto query = connection.qSelect(scope + R"SQL(
            SELECT it.tagid, count(DISTINCT i.imageid)
            FROM image_dates i JOIN mem.existing_files f ON f.path=i.path
            JOIN ImageTags it ON it.imageid=i.imageid
            GROUP BY it.tagid
        )SQL", bindings, {&id, &count});
        checkQuery(query);
        while (query->step())
        {
            auto tag = definitions.find(id.getValue());
            if (tag == definitions.end()) continue;
            result.push_back(tag->second);
            result.back().count = static_cast<size_t>(count.getValue());
        }
        checkQuery(query, true);
        std::sort(result.begin(), result.end(), tagLess);
        return result;
    }

    /** @brief Resolve a public tag's selected image without crossing the media scope. */
    TagIcon readTagIcon(int64_t tagId, const std::string &memoryURI)
    {
        checkQuery(connection.qExecute("ATTACH DATABASE :uri AS mem", {{":uri", std::make_shared<STRING>(memoryURI)}}));
        const auto definitions = tagDefinitions();
        const auto tag = definitions.find(tagId);
        if (tag == definitions.end() || tag->second.icon <= 0) throw Error(ErrorKind::NotFound, "Tag icon unavailable");
        STRING file, folder;
        auto input = bindings;
        input[":icon"] = std::make_shared<INT64>(tag->second.icon);
        input[":tag"] = std::make_shared<INT64>(tagId);
        auto query = connection.qSelect(scope + R"SQL(
            SELECT path,folder FROM image_dates WHERE imageid=:icon AND EXISTS (
                SELECT 1 FROM image_dates assigned JOIN mem.existing_files f ON f.path=assigned.path
                JOIN ImageTags it ON it.imageid=assigned.imageid WHERE it.tagid=:tag
            )
        )SQL", input, {&file, &folder});
        checkQuery(query);
        if (!query->step() || !exists(folder.getValue(), file.getValue()))
            throw Error(ErrorKind::NotFound, "Tag icon unavailable");
        TagIcon result;
        result.file = file.getValue();
        query.reset();
        std::string detail;
        if (connection.dbTableExist("ImageTagProperties"))
        {
            STRING region;
            auto face = connection.qSelect("SELECT value FROM ImageTagProperties WHERE imageid=:image AND tagid=:tag AND property='tagRegion' ORDER BY value LIMIT 1",
                {{":image", std::make_shared<INT64>(tag->second.icon)}, {":tag", std::make_shared<INT64>(tagId)}}, {&region});
            checkQuery(face);
            if (face->step())
            {
                // digiKam displays face rectangles with a ten-percent margin.
                const auto xml = region.getValue();
                auto attribute = [&](const std::string &name) -> int64_t {
                    std::smatch match;
                    if (!std::regex_search(xml, match, std::regex("\\b" + name + "=[\"']([0-9]{1,7})[\"']"))) return -1;
                    return std::stoll(match[1]);
                };
                const auto x = attribute("x"), y = attribute("y"), w = attribute("width"), h = attribute("height");
                if (x >= 0 && y >= 0 && w > 0 && h > 0)
                {
                    const auto margin = std::max(w, h) / 10;
                    const auto left = x - margin, top = y - margin;
                    result.crop = std::to_string(w + 2 * margin + std::min<int64_t>(0, left)) + "x" +
                        std::to_string(h + 2 * margin + std::min<int64_t>(0, top)) + "+" +
                        std::to_string(std::max<int64_t>(0, left)) + "+" + std::to_string(std::max<int64_t>(0, top));
                    detail = "detail:" + result.file + "?rect=" + std::to_string(left) + "," + std::to_string(top) + "-" +
                        std::to_string(w + 2 * margin) + "x" + std::to_string(h + 2 * margin);
                }
            }
        }
        // This optional cache must never prevent loading an icon from its original.
        SQLConnector_SQLite3 thumbs;
        thumbs.setThrowCPPErrorOnQueryFailure(false);
        const auto cache = database.parent_path() / "thumbnails-digikam.db";
        if (thumbs.connect("file:" + uriEncode(cache.string()) + "?mode=ro"))
        {
            BINARY bytes;
            const auto sql = detail.empty() ?
                "SELECT t.data FROM Thumbnails t JOIN FilePaths p ON p.thumbId=t.id WHERE p.path=:key AND COALESCE(t.orientationHint,0) IN (0,1) AND length(t.data) BETWEEN 1 AND 1048576" :
                "SELECT t.data FROM Thumbnails t JOIN CustomIdentifiers c ON c.thumbId=t.id WHERE c.identifier=:key AND length(t.data) BETWEEN 1 AND 1048576";
            auto cached = thumbs.qSelect(sql, {{":key", std::make_shared<STRING>(detail.empty() ? result.file : detail)}}, {&bytes});
            if (cached && cached->getLastSQLError().empty() && cached->step())
            {
                const auto data = bytes.toString();
                if (data.compare(0, 3, "\xff\xd8\xff") == 0) result.mime = "image/jpeg";
                else if (data.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0) result.mime = "image/png";
                // PGF thumbnails need a decoder; use the original below instead.
                if (!result.mime.empty()) result.data = data;
            }
        }
        return result;
    }

private:
    static bool tagLess(const Tag &a, const Tag &b)
    {
        return a.path == b.path ? a.id < b.id : a.path < b.path;
    }

    /** @brief Resolve public names and paths once, hiding technical branches.
     * Configured exclusions apply equally to the picker and informational badges.
     * Cycle detection also protects malformed digiKam hierarchies.
     */
    std::map<int64_t, Tag> tagDefinitions()
    {
        const auto hidden = [&](const std::string &name) {
            return std::any_of(exclusions.begin(), exclusions.end(), [&](const std::regex &pattern) {
                return std::regex_search(name, pattern);
            });
        };
        // digiKam marks technical tags with TagProperties.internalTag. Older
        // databases can also identify them through the reserved internal branch.
        std::unordered_set<int64_t> internalTags;
        if (connection.dbTableExist("TagProperties"))
        {
            INT64 tagId;
            auto query = connection.qSelect("SELECT tagid FROM TagProperties WHERE property='internalTag'", {}, {&tagId});
            checkQuery(query);
            while (query->step()) internalTags.insert(tagId.getValue());
            checkQuery(query, true);
        }
        std::map<int64_t, Tag> hierarchy;
        INT64 id, parent, icon;
        STRING name;
        {
            // Minimal/older catalogs may omit the optional icon column.
            bool hasIcon = false;
            STRING column;
            auto columns = connection.qSelect("SELECT name FROM pragma_table_info('Tags')", {}, {&column});
            checkQuery(columns);
            while (columns->step()) if (column.getValue() == "icon") hasIcon = true;
            checkQuery(columns, true);
            columns.reset();
            auto query = connection.qSelect("SELECT id, COALESCE(pid,0), COALESCE(name,''), " +
                std::string(hasIcon ? "COALESCE(icon,0)" : "0") + " FROM Tags", {}, {&id, &parent, &name, &icon});
            checkQuery(query);
            while (query->step()) hierarchy.emplace(id.getValue(), Tag{id.getValue(), parent.getValue(), name.getValue(), {}, 0, icon.getValue()});
            checkQuery(query, true);
        }
        std::map<int64_t, Tag> result;
        for (const auto &entry : hierarchy)
        {
            Tag tag = entry.second;
            if (tag.name.find_first_not_of(" \t\n\r") == std::string::npos) continue;
            tag.path = tag.name;
            if (hidden(tag.name)) continue;
            bool excluded = internalTags.count(tag.id) || tag.name == "_Digikam_Internal_Tags_" || tag.name == "_Digikam_root_tag_";
            std::unordered_set<int64_t> seen{tag.id};
            auto ancestor = hierarchy.find(tag.parent);
            while (ancestor != hierarchy.end() && ancestor->first != 0 && seen.insert(ancestor->first).second)
            {
                excluded = excluded || internalTags.count(ancestor->first) ||
                    ancestor->second.name == "_Digikam_Internal_Tags_" || hidden(ancestor->second.name);
                if (!ancestor->second.name.empty() && ancestor->second.name != "_Digikam_root_tag_")
                    tag.path = ancestor->second.name + " / " + tag.path;
                ancestor = hierarchy.find(ancestor->second.parent);
            }
            if (!excluded) result.emplace(tag.id, std::move(tag));
        }
        return result;
    }

    /** @brief Compile validated rules into parameterized SQL over Images i/info.
     * EXISTS keeps tag/metadata comparisons independent and avoids duplicate media
     * for multiple matching tags. Negation complements a total boolean predicate,
     * including absent rows/NULL values. Text contains uses instr, so '%' and '_'
     * are literal characters. Metadata tables are only referenced when requested.
     * @param filter Immutable allowlisted conditions; never raw client JSON.
     * @return Parenthesized AND/OR expression, or 1 for an empty filter.
     */
    std::string filterSQL(const TimelineFilter &filter)
    {
        return rulesSQL(filter.rules(), true);
    }

    /** @brief Compile each subfilter in parentheses; the main list always uses AND. */
    std::string rulesSQL(const std::vector<TimelineFilter::Rule> &rules, bool all)
    {
        std::string result;
        for (const auto &rule : rules)
        {
            if (!result.empty()) result += all ? " AND " : " OR ";
            if (rule.composed)
            {
                result += rulesSQL(rule.rules, rule.all);
                continue;
            }
            const std::string key = ":filter" + std::to_string(bindings.size());
            const std::string comparison = rule.op == "gt" ? ">" : rule.op == "lt" ? "<" : "=";
            std::string predicate;
            if (rule.field == "tag")
            {
                bindings[key] = std::make_shared<STRING>(std::string(rule.value.as_string()));
                predicate = "EXISTS (SELECT 1 FROM ImageTags it JOIN Tags t ON t.id=it.tagid WHERE it.imageid=i.id AND t.name=" + key + " COLLATE BINARY)";
            }
            else if (rule.field == "creationDate")
            {
                bindings[key] = std::make_shared<STRING>(std::string(rule.value.as_string()));
                predicate = "COALESCE((" + isoTimestampSQL("info.creationDate") + ") " + comparison + " (" + isoTimestampSQL(key) + "),0)";
            }
            else
            {
                // Rule construction is private to TimelineFilter::parse: the
                // column and operator below can only come from its allowlist.
                const std::string column = "metadata.\"" + rule.field + "\"";
                if (rule.op == "contains")
                {
                    bindings[key] = std::make_shared<STRING>(std::string(rule.value.as_string()));
                    predicate = "instr(lower(" + column + "),lower(" + key + "))>0";
                }
                else if (rule.value.is_string())
                {
                    bindings[key] = std::make_shared<STRING>(std::string(rule.value.as_string()));
                    predicate = column + "=" + key + " COLLATE BINARY";
                }
                else
                {
                    bindings[key] = std::make_shared<DOUBLE>(rule.value.to_number<double>());
                    predicate = column + comparison + key;
                }
                predicate = "EXISTS (SELECT 1 FROM ImageMetadata metadata WHERE metadata.imageid=i.id AND " + predicate + ")";
            }
            result += rule.negate ? "NOT (" + predicate + ")" : "(" + predicate + ")";
        }
        return result.empty() ? "1" : "(" + result + ")";
    }

    /** @brief Build a bound SQL scope from collection metadata, without loading Images.
     * Only the small AlbumRoots table is read here. Album joins, rootdir filtering,
     * ISO-date conversion and result ordering are performed by SQLite. The selected
     * timestamp is filename first, then ImageInformation.creationDate, then
     * Images.modificationDate; all ordering/range queries share that expression.
     * @param filter Validated SQL conditions applied to image_dates before grouping.
     * @param uriPath Optional file/folder subtree, relative to the HTTP root.
     * @param singleFile Whether uriPath selects one exact filename.
     */
    void prepareScope(const TimelineFilter &filter, const std::string &uriPath, bool singleFile)
    {
        std::string collections;
        {
            INT64 id;
            STRING identifier, specific;
            auto query = connection.qSelect("SELECT id, identifier, specificPath FROM AlbumRoots", {}, {&id, &identifier, &specific});
            checkQuery(query);
            size_t index = 0;
            while (query->step())
            {
                if (query->isNull(1) || query->isNull(2)) continue;
                auto collection = collectionPath(identifier.getValue(), specific.getValue());
                const auto relocated = overrides.find(collection.string());
                if (relocated != overrides.end()) collection = relocated->second;
                if (collection.empty() || (!inside(root, collection) && !inside(collection, root))) continue;
                const std::string key = std::to_string(index++);
                if (!collections.empty()) collections += ',';
                collections += "(:collection" + key + ",:path" + key + ")";
                bindings[":collection" + key] = std::make_shared<INT64>(id.getValue());
                bindings[":path" + key] = std::make_shared<STRING>(collection.string());
            }
            checkQuery(query, true);
        }
        if (collections.empty()) collections = "(NULL,NULL)";
        bindings[":root"] = std::make_shared<STRING>(root);
        bindings[":prefix"] = std::make_shared<STRING>(root == "/" ? "/" : root + "/");
        // Restrict folders/files before the CTE flattening barrier. In particular,
        // a thumbnail's mandatory check must not stream the entire library.
        std::string selected = "1";
        if (!uriPath.empty())
        {
            const auto full = normalized(fs::path(root) / fs::path(uriPath).relative_path());
            if (!inside(full, root)) throw Error(ErrorKind::InvalidArgument, "Invalid folder path");
            const auto folder = singleFile ? full.parent_path() : full;
            bindings[":selectedFolder"] = std::make_shared<STRING>(folder.string());
            if (singleFile)
            {
                bindings[":selectedName"] = std::make_shared<STRING>(full.filename().string());
                selected = "a.folder=:selectedFolder AND i.name=:selectedName";
            }
            else
            {
                bindings[":selectedPrefix"] = std::make_shared<STRING>(folder == "/" ? "/" : folder.string() + "/");
                selected = "a.folder=:selectedFolder OR substr(a.folder,1,length(:selectedPrefix))=:selectedPrefix";
            }
        }
        const auto predicate = filterSQL(filter);
        // These unbounded LIMIT/OFFSET clauses keep the date CTEs as streaming
        // subqueries. Without that flattening barrier SQLite substitutes the
        // filename/date expressions repeatedly into COALESCE, ORDER BY and
        // range filters, doing the same parsing many times per record. This
        // works without MATERIALIZED hints or a newer SQLite requirement.
        // CASE/COALESCE parse lower-priority dates only when needed; the original
        // modification timestamp is also returned separately for visible rows.
        scope = "WITH collections(id,path) AS (VALUES " + collections + R"SQL(),
            album_paths AS (
                SELECT a.id,
                    CASE WHEN trim(a.relativePath,'/')='' THEN c.path
                         ELSE rtrim(c.path,'/') || '/' || trim(a.relativePath,'/') END AS folder
                FROM Albums a JOIN collections c ON c.id=a.albumRoot
                WHERE a.relativePath IS NOT NULL
                  AND instr('/' || trim(a.relativePath,'/') || '/', '/../')=0
                  AND instr('/' || trim(a.relativePath,'/') || '/', '/./')=0
            ), image_dates AS (
                SELECT i.id AS imageid, a.folder || CASE WHEN a.folder='/' THEN '' ELSE '/' END || i.name AS path, a.folder,
                    i.modificationDate AS modification_date, info.creationDate AS creation_date,
        )SQL" + filenameDateSQL() + R"SQL( AS filename_date, COALESCE(i.fileSize,0) AS size
                FROM Images i JOIN album_paths a ON a.id=i.album
                LEFT JOIN ImageInformation info ON info.imageid=i.id
                WHERE (a.folder=:root OR substr(a.folder,1,length(:prefix))=:prefix)
                  AND i.name IS NOT NULL AND i.name NOT IN ('','.','..')
                  AND instr(i.name,'/')=0 AND instr(i.name,char(0))=0
                  AND ()SQL" + selected + ") AND " + predicate + R"SQL(
                LIMIT -1 OFFSET 0
            ), filename_times AS (
                SELECT imageid, path, folder, size, creation_date, modification_date,
        )SQL" + isoTimestampSQL("filename_date") + R"SQL( AS filename_time
                FROM image_dates
                LIMIT -1 OFFSET 0
            ), timestamps AS (
                SELECT imageid, path, folder, size, modification_date, filename_time,
                    CASE WHEN filename_time IS NULL THEN
        )SQL" + isoTimestampSQL("creation_date") + R"SQL( END AS creation_time
                FROM filename_times
                LIMIT -1 OFFSET 0
            ), media AS (
                SELECT imageid, path, folder, size,
                    COALESCE(filename_time, creation_time,
        )SQL" + isoTimestampSQL("modification_date") + ") AS timestamp, COALESCE(" +
                isoTimestampSQL("modification_date") + R"SQL(,0) AS mtime,
                    CASE WHEN filename_time IS NOT NULL THEN 'filename'
                         WHEN creation_time IS NOT NULL THEN 'creationDate'
                         ELSE 'modificationDate' END AS date_source
                FROM timestamps
            )
        )SQL";
    }

    /** @brief Validate each directory once and each file before exposing a row.
     * Directory and file symlinks are excluded to preserve rootdir containment.
     * @param directory Absolute database-derived album directory.
     * @param file Absolute candidate path under that directory.
     * @return Whether the candidate is an existing regular file within the scope.
     */
    bool exists(const std::string &directory, const std::string &file)
    {
        auto known = directories.find(directory);
        if (known == directories.end())
        {
            std::error_code ec;
            const bool valid = fs::canonical(directory, ec) == fs::path(directory) && !ec;
            known = directories.emplace(directory, valid).first;
        }
        std::error_code ec;
        return known->second && fs::is_regular_file(fs::symlink_status(file, ec)) && !ec;
    }

    const std::map<std::string, std::string> &overrides; ///< Immutable startup path corrections.
    const std::vector<std::regex> &exclusions; ///< Shared visibility rules for all tag output.
    std::map<int64_t, Tag> mediaTags; ///< Request-local public names/paths, shared by displayed events.
    bool mediaTagsLoaded = false;
    std::string root; ///< Canonical HTTP root for all statements in this request.
    fs::path database; ///< Nearest discovered database; no creation fallback.
    std::string stamp; ///< File/WAL identity before the read transaction.
    SQLConnector_SQLite3 connection; ///< Closed, with its transaction, at request end.
    Bindings bindings; ///< Collection IDs, collection paths and root scope parameters.
    std::string scope; ///< Reusable SELECT CTE; contains no interpolated user values.
    std::unordered_map<std::string, bool> directories; ///< Request-local directory validation results.
};

TimelineDatabase::TimelineDatabase(std::string root, int64_t gapSeconds, Mantids30::Runtime::Logs::AppLog *logger,
                                   std::vector<std::regex> tagExclusions, std::optional<TimelineFilter> prefilter)
    : root(fs::canonical(root).string()), gap(std::max<int64_t>(1, gapSeconds)),
      generation(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())), logger(logger),
      tagExclusions(std::move(tagExclusions)), prefilter(std::move(prefilter))
{
    try { collectionOverrides = relocatedCollections(this->root, logger); }
    catch (const Error &error)
    {
        if (logger) logger->log0("timeline", Mantids30::Runtime::Logs::WARNING,
            "Collection path detection unavailable: %s", error.what());
    }
    rescanTagFiles();
}

size_t TimelineDatabase::rescanTagFiles()
{
    std::lock_guard<std::mutex> scanLock(scanMutex);
    const auto started = std::chrono::steady_clock::now();
    if (logger) logger->log0("timeline", Mantids30::Runtime::Logs::INFO,
        "Tag file scan started: root=%s", root.c_str());
    static std::atomic<uint64_t> serial{0};
    auto next = std::make_shared<FileSnapshot>();
    next->uri = "file:umedia-tag-files-" + std::to_string(serial.fetch_add(1)) + "?mode=memory&cache=shared";
    auto &connection = next->connection;
    connection.setThrowCPPErrorOnQueryFailure(false);
    if (!sqlite3_compileoption_used("USE_URI") || !connection.connect(next->uri))
        throw Error(ErrorKind::Unavailable, "Cannot create the tag selector's memory database.");
    checkQuery(connection.qExecute("CREATE TABLE existing_files(path TEXT PRIMARY KEY) WITHOUT ROWID"));
    checkQuery(connection.qExecute("BEGIN"));
    size_t count = 0;
    try
    {
        // Do not follow directory symlinks; symlink files are excluded as well,
        // matching the timeline's containment rules. Store paths, never media metadata.
        for (const auto &entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied))
        {
            if (!fs::is_regular_file(entry.symlink_status())) continue;
            checkQuery(connection.qExecute("INSERT INTO existing_files(path) VALUES(:path)",
                {{":path", std::make_shared<STRING>(entry.path().string())}}));
            ++count;
        }
    }
    catch (const fs::filesystem_error &error)
    {
        throw Error(ErrorKind::Unavailable, "Gallery scan failed: " + std::string(error.what()));
    }
    checkQuery(connection.qExecute("COMMIT"));
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        fileSnapshot = std::move(next);
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (logger) logger->log0("timeline", Mantids30::Runtime::Logs::INFO,
        "Tag file scan finished: root=%s files=%zu elapsed=%.3f seconds", root.c_str(), count, elapsed);
    return count;
}

std::vector<TimelineDatabase::Tag> TimelineDatabase::tags() const
{
    std::shared_ptr<FileSnapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        snapshot = fileSnapshot;
    }
    Reader reader(root, generation.load(), effectiveFilter(), tagExclusions, collectionOverrides);
    auto result = reader.readTags(snapshot->uri);
    reader.verify();
    return result;
}

TimelineDatabase::TagIcon TimelineDatabase::tagIcon(int64_t id) const
{
    std::shared_ptr<FileSnapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        snapshot = fileSnapshot;
    }
    Reader reader(root, generation.load(), effectiveFilter(), tagExclusions, collectionOverrides);
    auto result = reader.readTagIcon(id, snapshot->uri);
    reader.verify();
    return result;
}

boost::json::object TimelineDatabase::filterValues() const
{
    std::shared_ptr<FileSnapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        snapshot = fileSnapshot;
    }
    Reader reader(root, generation.load(), effectiveFilter(), tagExclusions, collectionOverrides);
    auto result = reader.readFilterValues(snapshot->uri);
    reader.verify();
    return result;
}

TimelineDatabase::FolderView TimelineDatabase::folders(const std::string &path) const
{
    Reader reader(root, generation.load(), effectiveFilter(), tagExclusions, collectionOverrides, path.empty() ? "/" : path);
    FolderView result;
    reader.walkPaths([&](const std::string &file) {
        result.files.insert(file);
        for (auto parent = fs::path(file).parent_path();; parent = parent.parent_path())
        {
            result.directories.insert(parent.string());
            if (parent == "/") break;
        }
    });
    reader.verify();
    return result;
}

bool TimelineDatabase::allowsFile(const std::string &path) const
{
    Reader reader(root, generation.load(), effectiveFilter(), tagExclusions, collectionOverrides, path, true);
    bool allowed = false;
    reader.walkPaths([&](const std::string &) { allowed = true; });
    reader.verify();
    return allowed;
}

TimelineDatabase::FeedPage TimelineDatabase::feed(size_t cursor, size_t limit, const std::string &revision, const TimelineFilter &filter, bool ascending) const
{
    if (!limit || limit > 12) throw Error(ErrorKind::InvalidArgument, "Invalid page size");
    if (cursor && revision.empty()) throw Error(ErrorKind::Stale, "Timeline changed. Refresh to continue.");
    const auto version = generation.load();
    try
    {
        Reader reader(root, version, effectiveFilter(filter), tagExclusions, collectionOverrides);
        reader.requireRevision(revision);
        auto page = reader.readFeed(cursor, limit, gap, ascending);
        reader.verify();
        if (generation.load() != version) throw Error(ErrorKind::Stale, "Timeline changed. Refresh to continue.");
        return page;
    }
    catch (const Error &error)
    {
        if (error.kind != ErrorKind::Unavailable) throw;
        FeedPage page;
        page.revision = std::to_string(version);
        page.error = error.what();
        return page;
    }
}

TimelineDatabase::EventPage TimelineDatabase::event(const std::string &id, size_t cursor, size_t limit, const TimelineFilter &filter) const
{
    if (!limit || limit > 60) throw Error(ErrorKind::InvalidArgument, "Invalid page size");
    const auto version = generation.load();
    Reader reader(root, version, effectiveFilter(filter), tagExclusions, collectionOverrides);
    auto page = reader.readEvent(reader.decodeEvent(id), cursor, limit, false);
    reader.verify();
    if (generation.load() != version) throw Error(ErrorKind::Stale, "Timeline changed. Refresh to continue.");
    return page;
}

TimelineDatabase::Event TimelineDatabase::samples(const std::string &id, const TimelineFilter &filter) const
{
    const auto version = generation.load();
    Reader reader(root, version, effectiveFilter(filter), tagExclusions, collectionOverrides);
    auto event = reader.readEvent(reader.decodeEvent(id), 0, 0, true).event;
    reader.verify();
    if (generation.load() != version) throw Error(ErrorKind::Stale, "Timeline changed. Refresh to continue.");
    return event;
}
