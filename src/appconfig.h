#ifndef UMEDIABROWSER_APPCONFIG_H
#define UMEDIABROWSER_APPCONFIG_H

#include <boost/property_tree/info_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

// Group settings using native Boost INFO sections and ptree paths.
inline boost::property_tree::ptree readConfiguration(const std::string &filename)
{
    boost::property_tree::ptree config;
    boost::property_tree::read_info(filename, config);
    const boost::property_tree::ptree defaults = [] {
        boost::property_tree::ptree values;
        values.put("permissions.can-archive", "false");
        values.put("permissions.can-upload", "false");
        values.put("permissions.can-overwrite", "false");
        values.put("http.rootdir", ".");
        values.put("http.lport", "8001");
        values.put("http.ipv4", "true");
        values.put("http.laddr", "*");
        values.put("http.title", "");
        values.put("http.threads", "1024");
        values.put("http.use-x-real-ip", "false");
        values.put("timeline.timeline-gap-minutes", "30");
        values.put("timeline.tag-exclude-file", "");
        values.put("security.prefilter", "");
        values.put("security.passfile", "");
        values.put("logging.sys", "false");
        values.put("logging.debug", "false");
        values.put("tls.cafile", "");
        values.put("tls.keyfile", "");
        values.put("tls.certfile", "");
        values.put("tls.enabled", "false");
        return values;
    }();
    std::set<std::string> groups;
    for (const auto &group : config)
    {
        if (!defaults.count(group.first))
            throw std::runtime_error("Unknown configuration group: " + group.first);
        if (!groups.insert(group.first).second)
            throw std::runtime_error("Duplicate configuration group: " + group.first);
        if (!group.second.data().empty())
            throw std::runtime_error("Expected a configuration section: " + group.first);
        std::set<std::string> keys;
        const auto &expected = defaults.get_child(group.first);
        for (const auto &entry : group.second)
        {
            const auto path = group.first + "." + entry.first;
            if (!expected.count(entry.first))
                throw std::runtime_error("Unknown configuration key: " + path);
            if (!keys.insert(entry.first).second)
                throw std::runtime_error("Duplicate configuration key: " + path);
            if (!entry.second.empty())
                throw std::runtime_error("Expected a scalar configuration value: " + path);
        }
    }
    for (const auto &group : defaults)
        for (const auto &entry : group.second)
        {
            const auto path = group.first + "." + entry.first;
            if (!config.get_child_optional(path)) config.put(path, entry.second.data());
        }

    config.get<bool>("permissions.can-archive");
    config.get<bool>("permissions.can-upload");
    config.get<bool>("permissions.can-overwrite");
    config.get<bool>("http.ipv4");
    config.get<bool>("http.use-x-real-ip");
    config.get<bool>("logging.sys");
    config.get<bool>("logging.debug");
    config.get<bool>("tls.enabled");
    // Avoid unsigned conversion accepting negatives, overflow or trailing text.
    for (const auto *key : {"http.lport", "http.threads"})
    {
        const auto value = config.get<std::string>(key);
        unsigned int number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            number == 0 || number > std::numeric_limits<uint16_t>::max())
            throw std::runtime_error(std::string("Invalid ") + key + ": expected an integer from 1 to 65535");
    }
    if (config.get<bool>("tls.enabled"))
    {
        if (config.get<std::string>("tls.keyfile").empty() || config.get<std::string>("tls.certfile").empty())
            throw std::runtime_error("tls.enabled requires both tls.keyfile and tls.certfile");
    }
    return config;
}

#endif
