/** @file action_timeline.cpp
 *  @brief HTTP validation and JSON serialization for the timeline database layer.
 */
#include "../webclienthdlr.h"
#include <Mantids30/Protocol/HTTP/rsp_status.h>
#include <Mantids30/Core/Helpers/appexec.h>

#include <algorithm>
#include <filesystem>

using namespace Mantids30::Protocol;
using namespace Mantids30::Protocol::HTTP;

namespace {
/** @brief Parse an unsigned pagination argument with a bounded input length. */
bool number(const std::string &text, size_t &out, size_t fallback)
{
    out = fallback;
    if (text.empty()) return true;
    if (text.size() > 18 || text.find_first_not_of("0123456789") != std::string::npos) return false;
    try { out = std::stoull(text); return true; } catch (...) { return false; }
}

/** @brief Informational badges expose identity, short label and full ancestry. */
boost::json::array tagJSON(const std::vector<TimelineDatabase::Tag> &tags)
{
    boost::json::array result;
    for (const auto &tag : tags)
        result.push_back(boost::json::object{{"id", tag.id}, {"name", tag.name}, {"path", tag.path}});
    return result;
}

/** @brief Serialize one existing file and its event-wide carousel position. */
boost::json::object mediaJSON(const TimelineDatabase::Media &item)
{
    return {{"path", item.path}, {"folder", item.folder}, {"name", std::filesystem::path(item.path).filename().string()},
            {"type", "file"}, {"timestamp", item.timestamp}, {"mtime", item.mtime}, {"size", item.size},
            {"date_source", item.dateSource}, {"index", item.index}, {"tags", tagJSON(item.tags)}};
}

/** @brief Serialize a bounded page or sample list. */
boost::json::array mediaJSON(const std::vector<TimelineDatabase::Media> &items)
{
    boost::json::array result;
    for (const auto &item : items) result.push_back(mediaJSON(item));
    return result;
}

/** @brief Serialize event metadata independently of its optional samples. */
boost::json::object eventJSON(const TimelineDatabase::Event &event)
{
    return {{"id", event.id}, {"start", event.start}, {"end", event.end}, {"count", event.count}, {"tags", tagJSON(event.tags)}};
}
}

/** @brief Adapt HTTP requests to direct read-only database operations.
 * SQL, discovery, filesystem filtering and event calculations belong entirely
 * to TimelineDatabase. This adapter only handles HTTP methods, limits and JSON.
 * Every service accepts the same URL-encoded JSON `filter` query parameter.
 * Parsing precedes invalidation/database access; invalid filters return HTTP 400.
 * The database always adds the immutable startup prefilter with AND.
 */
Status::Code WebClientHdlr::action_timeline()
{
    auto vars = clientRequest.getVarsBySource(HTTP::Source::GET);
    const auto act = vars->getStringValue("act");
    auto finish = [&](Status::Code code, const std::string &error, const boost::json::value &data = boost::json::value{}) {
        writeJSON(code == Status::Code::S_200_OK, error, data);
        return respond(code, clientRequest.getURI());
    };
    const bool refresh = act == "timeline_refresh";
    const bool rescan = act == "timeline_tags_refresh";
    if (clientRequest.requestLine.getHTTPMethod() != (refresh || rescan ? "POST" : "GET"))
        return finish(Status::Code::S_405_METHOD_NOT_ALLOWED, "Method not allowed");
    if (!webClientParameters.timeline)
        return finish(Status::Code::S_503_SERVICE_UNAVAILABLE, "Timeline database unavailable");

    size_t cursor, limit;
    if (!number(vars->getStringValue("cursor"), cursor, 0) || !number(vars->getStringValue("limit"), limit, act == "timeline_event" ? 60 : 8))
        return finish(Status::Code::S_400_BAD_REQUEST, "Invalid pagination");
    limit = std::clamp<size_t>(limit, 1, act == "timeline_event" ? 60 : 12);
    try
    {
        const auto filter = TimelineFilter::parse(vars->getStringValue("filter"));
        auto &database = *webClientParameters.timeline;
        if (act == "timeline_tag_icon")
        {
            size_t id;
            if (!number(vars->getStringValue("id"), id, 0) || id == 0 || id > INT64_MAX)
                return finish(Status::Code::S_400_BAD_REQUEST, "Invalid tag ID");
            auto icon = database.tagIcon(static_cast<int64_t>(id));
            if (icon.data.empty())
            {
                Mantids30::Core::Helpers::AppSpawn process;
                if (!process.setExec("/usr/bin/convert"))
                    return finish(Status::Code::S_404_NOT_FOUND, "Tag icon unavailable");
                std::vector<std::string> args{icon.file + "[0]", "-auto-orient"};
                if (!icon.crop.empty()) args.insert(args.end(), {"-crop", icon.crop, "+repage"});
                args.insert(args.end(), {"-thumbnail", "80x80>", "-strip", "-quality", "80", "jpg:-"});
                for (const auto &arg : args) process.addArgument(arg);
                if (!process.spawnProcess(true, false))
                    return finish(Status::Code::S_404_NOT_FOUND, "Tag icon unavailable");
                char buffer[8192];
                ssize_t length;
                while ((length = process.read(1, buffer, sizeof(buffer))) > 0) icon.data.append(buffer, length);
                process.waitUntilProcessEnds();
                if (icon.data.compare(0, 3, "\xff\xd8\xff") != 0)
                    return finish(Status::Code::S_404_NOT_FOUND, "Tag icon unavailable");
                icon.mime = "image/jpeg";
            }
            serverResponse.contentType = icon.mime;
            serverResponse.headers.replace("Cache-Control", "private, max-age=300");
            getResponseContentStreamableObject()->writeString(icon.data);
            return respond(Status::Code::S_200_OK, clientRequest.getURI());
        }
        if (act == "current_filter")
        {
            // An exported object must be accepted unchanged by --prefilter.
            const auto effective = database.effectiveFilter(filter);
            TimelineFilter::parsePrefilter(boost::json::serialize(effective.json()));
            return finish(Status::Code::S_200_OK, "", effective.json());
        }
        if (act == "timeline_filter_values")
            return finish(Status::Code::S_200_OK, "", boost::json::object{{"values", database.filterValues()}});
        if (act == "timeline_tags" || rescan)
        {
            size_t scanned = 0;
            if (rescan) scanned = database.rescanTagFiles();
            boost::json::array tags;
            for (const auto &tag : database.tags())
                tags.push_back(boost::json::object{{"id", tag.id}, {"parent_id", tag.parent},
                    {"name", tag.name}, {"path", tag.path}, {"count", tag.count},
                    {"icon", tag.icon > 0 ? "?act=timeline_tag_icon&id=" + std::to_string(tag.id) : ""}});
            boost::json::object result{{"tags", std::move(tags)}};
            if (rescan) result["scanned_files"] = scanned;
            return finish(Status::Code::S_200_OK, "", result);
        }
        const auto order = vars->getStringValue("order");
        if (!order.empty() && order != "asc" && order != "desc")
            return finish(Status::Code::S_400_BAD_REQUEST, "Invalid timeline order");
        if (refresh) database.invalidate();
        if (act == "timeline" || refresh)
        {
            const auto page = database.feed(cursor, limit, vars->getStringValue("revision"), filter, order == "asc");
            boost::json::array events;
            for (const auto &event : page.events)
            {
                auto json = eventJSON(event);
                json["samples"] = mediaJSON(event.samples);
                events.push_back(std::move(json));
            }
            return finish(Status::Code::S_200_OK, "", boost::json::object{
                {"order", order == "asc" ? "asc" : "desc"}, {"revision", page.revision}, {"events", std::move(events)}, {"next", page.next}, {"has_more", page.hasMore},
                {"total_events", page.totalEvents}, {"total_items", page.totalItems},
                {"database_available", page.databaseAvailable}, {"error", page.error}, {"skipped", page.skipped},
                {"gap_seconds", database.gapSeconds()}, {"filter", filter.json()},
                {"prefilter_active", database.hasPrefilter()}});
        }
        if (act == "timeline_samples")
        {
            const auto event = database.samples(vars->getStringValue("event"), filter);
            auto json = eventJSON(event);
            json["samples"] = mediaJSON(event.samples);
            return finish(Status::Code::S_200_OK, "", json);
        }
        const auto page = database.event(vars->getStringValue("event"), cursor, limit, filter);
        auto json = eventJSON(page.event);
        json["filter"] = filter.json();
        json["prefilter_active"] = database.hasPrefilter();
        json["items"] = mediaJSON(page.items);
        json["next"] = page.next;
        json["has_more"] = page.hasMore;
        return finish(Status::Code::S_200_OK, "", json);
    }
    catch (const std::invalid_argument &error)
    {
        return finish(Status::Code::S_400_BAD_REQUEST, error.what());
    }
    catch (const TimelineDatabase::Error &error)
    {
        switch (error.kind)
        {
        case TimelineDatabase::ErrorKind::InvalidArgument:
            return finish(Status::Code::S_400_BAD_REQUEST, error.what());
        case TimelineDatabase::ErrorKind::NotFound:
            return finish(Status::Code::S_404_NOT_FOUND, error.what());
        case TimelineDatabase::ErrorKind::Stale:
            return finish(Status::Code::S_409_CONFLICT, error.what(), boost::json::object{{"stale", true}});
        case TimelineDatabase::ErrorKind::Unavailable:
            return finish(Status::Code::S_503_SERVICE_UNAVAILABLE, error.what());
        }
    }
    return finish(Status::Code::S_503_SERVICE_UNAVAILABLE, "Timeline database unavailable");
}
