/** @file timeline_database.cpp
 *  @brief Streaming implementation of the read-only timeline database layer.
 */
#include "timeline_database.h"
#include <Mantids30/Runtime/Logs/applog.h>
#include <Mantids30/DB/SQLite3/sqlconnector_sqlite3.h>
#include <Mantids30/Core/Memory/a_int64.h>
#include <Mantids30/Core/Memory/a_string.h>
#include <Mantids30/Core/Memory/a_double.h>

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
 */
class TimelineDatabase::Reader
{
public:
    /** @brief Open a URI-enforced read-only connection and begin a read transaction.
     * @param root Canonical HTTP root.
     * @param version Revision counter captured by the calling API method.
     * @param filter Validated conditions included in SQL scope and revision identity.
     * @throws Error If discovery, connection, schema access or URI support fails.
     */
    Reader(const std::string &root, uint64_t version, const TimelineFilter &filter) : root(root), database(findDatabase(root))
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
        prepareScope(filter);
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
     * @return Number of SQL candidates, including skipped records.
     */
    size_t walk(const Event *range, bool byFolder, const std::function<void(Media)> &consume, size_t &skipped)
    {
        auto input = bindings;
        std::string sql = scope + " SELECT path, folder, timestamp, size, mtime, date_source FROM media";
        if (range)
        {
            sql += " WHERE timestamp BETWEEN :start AND :end";
            input[":start"] = std::make_shared<INT64>(range->start);
            input[":end"] = std::make_shared<INT64>(range->end);
        }
        sql += byFolder ? " ORDER BY folder, timestamp DESC, path" : " ORDER BY timestamp DESC, path";
        STRING path, folder, dateSource;
        INT64 timestamp, size, modified;
        auto query = connection.qSelect(sql, input, {&path, &folder, &timestamp, &size, &modified, &dateSource});
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
            consume(std::move(item));
        }
        checkQuery(query, true);
        return candidates;
    }

    /** @brief Stream time-ordered records, retaining only the requested event page.
     * @param cursor Number of complete events to skip.
     * @param limit Maximum event summaries to retain.
     * @param gap Inactivity threshold starting a new event; equality starts a new group.
     * @return Requested page with exact existing-file and event totals.
     */
    FeedPage readFeed(size_t cursor, size_t limit, int64_t gap)
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
            if (!current.count || previous - item.timestamp >= gap)
            {
                finish();
                ++page.totalEvents;
                current.end = item.timestamp;
            }
            current.start = previous = item.timestamp;
            ++current.count;
            ++page.totalItems;
        }, page.skipped);
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
        EventPage page;
        page.event = std::move(event);
        page.event.count = 0;
        size_t skipped = 0;
        thread_local std::mt19937_64 random(std::random_device{}());
        const size_t candidates = walk(&page.event, true, [&](Media item) {
            item.index = page.event.count++;
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
        if (!candidates) throw Error(ErrorKind::NotFound, "Event not found");
        if (cursor > page.event.count) throw Error(ErrorKind::InvalidArgument, "Invalid cursor");
        page.next = cursor + page.items.size();
        page.hasMore = page.next < page.event.count;
        if (randomSamples) std::shuffle(page.event.samples.begin(), page.event.samples.end(), random);
        return page;
    }

    std::string revision; ///< Token for the currently open database transaction.

    /** @brief Join the current digiKam rows to an immutable existing-path table.
     * Ancestors provide labels only; a parent without assigned files is not an
     * option. Missing dates do not disqualify a file from the tag catalog.
     */
    std::vector<Tag> readTags(const std::string &memoryURI, const std::vector<std::regex> &exclusions)
    {
        const auto hidden = [&](const std::string &name) {
            return std::any_of(exclusions.begin(), exclusions.end(), [&](const std::regex &pattern) {
                return std::regex_search(name, pattern);
            });
        };
        checkQuery(connection.qExecute("ATTACH DATABASE :uri AS mem", {{":uri", std::make_shared<STRING>(memoryURI)}}));
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
        INT64 id, parent, count;
        STRING name;
        {
            auto query = connection.qSelect("SELECT id, COALESCE(pid,0), COALESCE(name,'') FROM Tags", {}, {&id, &parent, &name});
            checkQuery(query);
            while (query->step()) hierarchy.emplace(id.getValue(), Tag{id.getValue(), parent.getValue(), name.getValue(), {}, 0});
            checkQuery(query, true);
        }
        std::vector<Tag> result;
        auto query = connection.qSelect(scope + R"SQL(
            SELECT t.id, COALESCE(t.pid,0), t.name, count(DISTINCT i.imageid)
            FROM image_dates i JOIN mem.existing_files f ON f.path=i.path
            JOIN ImageTags it ON it.imageid=i.imageid JOIN Tags t ON t.id=it.tagid
            WHERE trim(COALESCE(t.name,''), char(9)||char(10)||char(13)||' ')<>''
            GROUP BY t.id, t.pid, t.name
        )SQL", bindings, {&id, &parent, &name, &count});
        checkQuery(query);
        while (query->step())
        {
            Tag tag{id.getValue(), parent.getValue(), name.getValue(), name.getValue(), static_cast<size_t>(count.getValue())};
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
            if (!excluded) result.push_back(std::move(tag));
        }
        checkQuery(query, true);
        std::sort(result.begin(), result.end(), [](const Tag &a, const Tag &b) {
            return a.path == b.path ? a.id < b.id : a.path < b.path;
        });
        return result;
    }

private:
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
     */
    void prepareScope(const TimelineFilter &filter)
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
                const auto collection = collectionPath(identifier.getValue(), specific.getValue());
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
                  AND )SQL" + predicate + R"SQL(
                LIMIT -1 OFFSET 0
            ), filename_times AS (
                SELECT path, folder, size, creation_date, modification_date,
        )SQL" + isoTimestampSQL("filename_date") + R"SQL( AS filename_time
                FROM image_dates
                LIMIT -1 OFFSET 0
            ), timestamps AS (
                SELECT path, folder, size, modification_date, filename_time,
                    CASE WHEN filename_time IS NULL THEN
        )SQL" + isoTimestampSQL("creation_date") + R"SQL( END AS creation_time
                FROM filename_times
                LIMIT -1 OFFSET 0
            ), media AS (
                SELECT path, folder, size,
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

    std::string root; ///< Canonical HTTP root for all statements in this request.
    fs::path database; ///< Nearest discovered database; no creation fallback.
    std::string stamp; ///< File/WAL identity before the read transaction.
    SQLConnector_SQLite3 connection; ///< Closed, with its transaction, at request end.
    Bindings bindings; ///< Collection IDs, collection paths and root scope parameters.
    std::string scope; ///< Reusable SELECT CTE; contains no interpolated user values.
    std::unordered_map<std::string, bool> directories; ///< Request-local directory validation results.
};

TimelineDatabase::TimelineDatabase(std::string root, int64_t gapSeconds, Mantids30::Runtime::Logs::AppLog *logger,
                                   std::vector<std::regex> tagExclusions)
    : root(fs::canonical(root).string()), gap(std::max<int64_t>(1, gapSeconds)),
      generation(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())), logger(logger),
      tagExclusions(std::move(tagExclusions))
{
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
    Reader reader(root, generation.load(), {});
    auto result = reader.readTags(snapshot->uri, tagExclusions);
    reader.verify();
    return result;
}

TimelineDatabase::FeedPage TimelineDatabase::feed(size_t cursor, size_t limit, const std::string &revision, const TimelineFilter &filter) const
{
    if (!limit || limit > 12) throw Error(ErrorKind::InvalidArgument, "Invalid page size");
    if (cursor && revision.empty()) throw Error(ErrorKind::Stale, "Timeline changed. Refresh to continue.");
    const auto version = generation.load();
    try
    {
        Reader reader(root, version, filter);
        reader.requireRevision(revision);
        auto page = reader.readFeed(cursor, limit, gap);
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
    Reader reader(root, version, filter);
    auto page = reader.readEvent(reader.decodeEvent(id), cursor, limit, false);
    reader.verify();
    if (generation.load() != version) throw Error(ErrorKind::Stale, "Timeline changed. Refresh to continue.");
    return page;
}

TimelineDatabase::Event TimelineDatabase::samples(const std::string &id, const TimelineFilter &filter) const
{
    const auto version = generation.load();
    Reader reader(root, version, filter);
    auto event = reader.readEvent(reader.decodeEvent(id), 0, 0, true).event;
    reader.verify();
    if (generation.load() != version) throw Error(ErrorKind::Stale, "Timeline changed. Refresh to continue.");
    return event;
}
