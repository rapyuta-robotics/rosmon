// Log event with metadata
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#ifndef ROSMON_LOG_EVENT_H
#define ROSMON_LOG_EVENT_H

#include <string>

namespace rosmon
{

struct LogEvent
{
public:
	enum class Type
	{
		/**
		 * Raw messages from monitored nodes. These are self-coded using
		 * ANSI escape codes. */
		Raw,

		Info,
		Warning,
		Error
	};

	LogEvent(std::string source, std::string message, Type type = Type::Raw)
	 : source{std::move(source)}, message{std::move(message)}, type{type}
	{}

	std::string source;
	std::string message;
	Type type;
};

inline std::string toString(LogEvent::Type type) {
	switch(type) {
		case LogEvent::Type::Info:
			return " INFO";
		case LogEvent::Type::Warning:
			return " WARN";
		case LogEvent::Type::Error:
			return "ERROR";
		default:
			return "DEBUG";
	}
}

}

#endif
