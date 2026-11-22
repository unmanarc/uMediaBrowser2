#pragma once

/** @file timeline_filter.h
 *  @brief Validated, stateless JSON filters shared by the timeline API and editor.
 */

#include <boost/json.hpp>
#include <string>
#include <vector>

/** @brief A bounded AND list of conditions or composed AND/OR subfilters.
 *
 * Wire format: {"rules":[{"match":"any","rules":[{"field":"tag","op":"eq",
 * "value":"Aaron"},{"field":"tag","op":"eq","value":"Esther"}]}]}.
 * The main list always uses AND; only subfilters select all/any. Empty main rules
 * match the whole library; subfilters must be nonempty, at most four levels deep.
 * Only parse() can populate conditions, so SQL identifiers/operators always come
 * from a validated allowlist. Values remain data and must be bound by the reader.
 * Instances contain no per-user server state and never affect the folder browser.
 */
class TimelineFilter
{
public:
    /** @brief One comparison (optionally negated) or a composed subfilter. */
    struct Rule
    {
        std::string field; ///< Allowlisted tag, creationDate or ImageMetadata column.
        std::string op; ///< eq, contains, gt or lt, restricted by field type.
        boost::json::value value; ///< Nonempty text/ISO date or finite JSON number.
        bool negate = false; ///< True matches files for which the comparison is false/missing.
        bool composed = false; ///< True selects a subfilter instead of a comparison.
        bool all = true; ///< Subfilter joins children with AND (true) or OR (false).
        std::vector<Rule> rules; ///< Validated children of a nonempty composed subfilter.
    };

    /** @brief Construct an empty filter matching all existing timeline files. */
    TimelineFilter() = default;

    /** @brief Parse at most 8192 UTF-8 bytes and 20 items, including subfilters.
     * @param json Decoded JSON query parameter; empty means no conditions.
     * @return Validated conditions with normalized dates and stable key ordering.
     * @throws std::invalid_argument For malformed JSON, fields, operators or values.
     */
    static TimelineFilter parse(const std::string &json);

    /** @brief Describe every filterable field for the editor, without reading tags.
     * @return Objects containing field, label, type and explanatory hint.
     * @note ImageMetadata types follow digiKam's SQLite schema; enums are numbers.
     */
    static boost::json::array fields();

    /** @return Canonical JSON object suitable for API responses and client editing. */
    boost::json::object json() const;

    /** @return Stable identity for revision scoping; empty for any empty rule list. */
    std::string identity() const;

    /** @return Immutable conditions; all identifiers and operators are validated. */
    const std::vector<Rule> &rules() const { return conditions; }

private:
    static std::vector<Rule> parseRules(const boost::json::array &rules, size_t depth, size_t &count);
    std::vector<Rule> conditions; ///< Main AND list, preserving editor order.
};
