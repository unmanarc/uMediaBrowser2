/** @file timeline_filter.cpp
 *  @brief Strict JSON validation and digiKam field catalog for timeline filtering.
 */
#include "timeline_filter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <stdexcept>

namespace {
/** @brief Trusted field names/types from digiKam core/data/database/dbconfig.xml.cmake.in.
 * https://github.com/KDE/digikam/blob/master/core/data/database/dbconfig.xml.cmake.in
 * No user-provided column names can enter the SQL compiler.
 */
struct Field
{
    const char *name; ///< JSON field and allowlisted database column (except tag/date).
    const char *label; ///< Human-readable editor label.
    const char *type; ///< tag, date, text or number.
    const char *hint; ///< Units or matching semantics, visible alongside the rule.
};

/** @brief Catalog shared by parser validation and the embedded editor. */
const Field catalog[] = {
    {"tag", "Tag", "tag", "Choose a tag assigned to existing files. Branches are shown as paths; matching uses the exact name in any branch. Rescan to update files added or removed since the last scan."},
    {"creationDate", "Creation date", "date", "Uses digiKam creationDate only, never the filename or modification date. Dates without a timezone use server local time."},
    {"make", "Camera manufacturer", "text", "Finds this text anywhere in the field (ignores A–Z letter case)."},
    {"model", "Camera model", "text", "Finds this text anywhere in the field (ignores A–Z letter case)."},
    {"lens", "Lens", "text", "Finds this text anywhere in the field (ignores A–Z letter case)."},
    {"aperture", "Aperture", "number", "f-number, for example 2.8."},
    {"focalLength", "Focal length", "number", "Millimeters (mm)."},
    {"focalLength35", "Focal length (35 mm equivalent)", "number", "Millimeters (mm)."},
    {"exposureTime", "Exposure time", "number", "Seconds; enter 0.008 for 1/125 s."},
    {"exposureProgram", "Exposure program", "number", "Numeric code stored by digiKam."},
    {"exposureMode", "Exposure mode", "number", "Numeric code stored by digiKam."},
    {"sensitivity", "ISO sensitivity", "number", "ISO value, for example 400."},
    {"flash", "Flash", "number", "Numeric flash bitmask stored by digiKam."},
    {"whiteBalance", "White balance", "number", "Numeric code stored by digiKam."},
    {"whiteBalanceColorTemperature", "White balance temperature", "number", "Kelvin (K)."},
    {"meteringMode", "Metering mode", "number", "Numeric code stored by digiKam."},
    {"subjectDistance", "Subject distance", "number", "Meters (m)."},
    {"subjectDistanceCategory", "Subject distance category", "number", "Numeric code stored by digiKam."}
};

/** @brief Raise a consistent validation failure for the HTTP adapter. */
[[noreturn]] void invalid(const std::string &reason)
{
    throw std::invalid_argument("Invalid timeline filter: " + reason);
}

/** @brief Reject misspelled or unsupported properties instead of silently ignoring them. */
void keys(const boost::json::object &object, std::initializer_list<boost::json::string_view> allowed)
{
    for (const auto &entry : object)
        if (std::find(allowed.begin(), allowed.end(), entry.key()) == allowed.end())
            invalid("unknown property " + std::string(entry.key()));
}

/** @brief Require a JSON string without converting unrelated JSON types. */
std::string string(const boost::json::object &object, const char *key)
{
    const auto *value = object.if_contains(key);
    if (!value || !value->is_string()) invalid(std::string(key) + " must be a string");
    return std::string(value->as_string());
}

/** @brief Validate an ISO calendar/time, normalizing date-only/minute-only inputs.
 * Accepts years 0001..9999, seconds with optional fraction, Z and numeric offsets.
 * Offsets must be within +/-14:00. Calendar dates are checked independently of
 * SQLite normalization. SQL comparisons have one-second precision.
 */
std::string date(const std::string &value)
{
    static const std::regex iso(R"(^([0-9]{4})-([0-9]{2})-([0-9]{2})([T ]([0-9]{2}):([0-9]{2})(:([0-9]{2})(\.[0-9]{1,9})?)?(Z|[+-][0-9]{2}:[0-9]{2})?)?$)");
    std::smatch parts;
    if (!std::regex_match(value, parts, iso)) invalid("creationDate must be an ISO date or date/time");
    const int year = std::stoi(parts[1]), month = std::stoi(parts[2]), day = std::stoi(parts[3]);
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (!year || month < 1 || month > 12 || day < 1 || day > days[month - 1] + (month == 2 && leap))
        invalid("creationDate has an invalid calendar date");
    if (!parts[4].matched) return value + "T00:00:00";
    if (std::stoi(parts[5]) > 23 || std::stoi(parts[6]) > 59 || (parts[8].matched && std::stoi(parts[8]) > 59))
        invalid("creationDate has an invalid time");
    const auto zone = parts[10].str();
    if (zone.size() > 1)
    {
        const int hours = std::stoi(zone.substr(1,2)), minutes = std::stoi(zone.substr(4,2));
        if (hours > 14 || minutes > 59 || (hours == 14 && minutes)) invalid("invalid timezone offset");
    }
    return value.substr(0,10) + "T" + parts[5].str() + ":" + parts[6].str() +
           (parts[7].matched ? parts[7].str() : ":00") + zone;
}
}

TimelineFilter TimelineFilter::parse(const std::string &text)
{
    if (text.empty()) return {};
    return parse(text, 8192, 20);
}

TimelineFilter TimelineFilter::parsePrefilter(const std::string &text)
{
    return parse(text, 65536, 200);
}

TimelineFilter TimelineFilter::andFilter(const TimelineFilter &other) const
{
    auto result = *this;
    result.conditions.insert(result.conditions.end(), other.conditions.begin(), other.conditions.end());
    return result;
}

TimelineFilter TimelineFilter::parse(const std::string &text, size_t maxBytes, size_t maxItems)
{
    TimelineFilter result;
    if (text.size() > maxBytes) invalid("maximum JSON size is " + std::to_string(maxBytes) + " bytes");
    boost::system::error_code error;
    const auto parsed = boost::json::parse(text, error);
    if (error || !parsed.is_object()) invalid("expected a JSON object");
    const auto &object = parsed.as_object();
    keys(object, {"match", "rules"});
    if (object.if_contains("match") && string(object, "match") != "all")
        invalid("the main filter always uses AND; use an any subfilter for OR");
    const auto *rules = object.if_contains("rules");
    if (!rules || !rules->is_array()) invalid("rules must be an array");
    size_t count = 0;
    result.conditions = parseRules(rules->as_array(), 0, count, maxItems);
    return result;
}

std::vector<TimelineFilter::Rule> TimelineFilter::parseRules(const boost::json::array &rules, size_t depth, size_t &count, size_t maxItems)
{
    std::vector<Rule> conditions;
    for (const auto &value : rules)
    {
        if (++count > maxItems) invalid("maximum is " + std::to_string(maxItems) + " items including subfilters");
        if (!value.is_object()) invalid("each rule must be an object");
        const auto &rule = value.as_object();
        Rule condition;
        if (rule.if_contains("rules") || rule.if_contains("match"))
        {
            keys(rule, {"match", "rules"});
            const auto match = string(rule, "match");
            if (match != "all" && match != "any") invalid("subfilter match must be all or any");
            const auto *children = rule.if_contains("rules");
            if (!children || !children->is_array() || children->as_array().empty())
                invalid("subfilter rules must be a nonempty array");
            if (depth >= 4) invalid("maximum subfilter depth is 4");
            condition.composed = true;
            condition.all = match == "all";
            condition.rules = parseRules(children->as_array(), depth + 1, count, maxItems);
            conditions.push_back(std::move(condition));
            continue;
        }
        keys(rule, {"field", "op", "value", "negate"});
        condition.field = string(rule, "field");
        condition.op = string(rule, "op");
        const auto field = std::find_if(std::begin(catalog), std::end(catalog), [&](const Field &f) { return condition.field == f.name; });
        if (field == std::end(catalog)) invalid("unknown field " + condition.field);
        if (const auto *negate = rule.if_contains("negate"))
        {
            if (!negate->is_bool()) invalid("negate must be boolean");
            condition.negate = negate->as_bool();
        }
        const auto *input = rule.if_contains("value");
        if (!input) invalid("value is required");
        const std::string type = field->type;
        if (type == "number")
        {
            if (condition.op != "gt" && condition.op != "lt" && condition.op != "eq") invalid("numeric operator must be gt, lt or eq");
            if (!input->is_number() || !std::isfinite(input->to_number<double>())) invalid("value must be a finite JSON number");
            condition.value = input->to_number<double>();
        }
        else
        {
            auto textValue = string(rule, "value");
            if (textValue.empty() || textValue.size() > 256 || textValue.find('\0') != std::string::npos ||
                std::all_of(textValue.begin(), textValue.end(), [](unsigned char c) { return std::isspace(c); }))
                invalid("text values must contain 1..256 bytes of nonblank text without NUL");
            if (type == "date")
            {
                if (condition.op != "gt" && condition.op != "lt") invalid("date operator must be gt or lt");
                textValue = date(textValue);
            }
            else if (condition.op != (type == "tag" ? "eq" : "contains")) invalid("unsupported text/tag operator");
            condition.value = textValue;
        }
        conditions.push_back(std::move(condition));
    }
    return conditions;
}

boost::json::array TimelineFilter::fields()
{
    boost::json::array result;
    for (const auto &field : catalog)
        result.push_back({{"field", field.name}, {"label", field.label}, {"type", field.type}, {"hint", field.hint}});
    return result;
}

namespace {
/** @brief Preserve the tree and normalize every leaf for stable revision identity. */
boost::json::array rulesJSON(const std::vector<TimelineFilter::Rule> &conditions)
{
    boost::json::array rules;
    for (const auto &rule : conditions)
        if (rule.composed)
            rules.push_back({{"match", rule.all ? "all" : "any"}, {"rules", rulesJSON(rule.rules)}});
        else
            rules.push_back({{"field", rule.field}, {"op", rule.op}, {"value", rule.value}, {"negate", rule.negate}});
    return rules;
}
}

boost::json::object TimelineFilter::json() const
{
    return {{"rules", rulesJSON(conditions)}};
}

std::string TimelineFilter::identity() const
{
    return conditions.empty() ? "" : boost::json::serialize(json());
}
