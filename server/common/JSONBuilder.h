/*
 * Copyright (C) 2013 Project Hatohol
 *
 * This file is part of Hatohol.
 *
 * Hatohol is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * Hatohol is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Hatohol. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef JSONBuilder_h
#define JSONBuilder_h

#include <string>
#include <json-glib/json-glib.h>

class JSONBuilder
{
public:
	JSONBuilder(void);
	~JSONBuilder();
	std::string generate(void);
	void startObject(const char *member = NULL);
	void startObject(const std::string &member);
	void endObject(void);
	void startArray(const std::string &member);
	void endArray(void);
	void add(const std::string &member, const std::string &value);
	void add(const std::string &member, gint64 value);
	void add(const gint64 value);
	void add(const std::string &value);
	void addTrue(const std::string &member);
	void addFalse(const std::string &member);
	void addNull(const std::string &member);

private:
	JsonBuilder *m_builder;
};

#endif // JSONBuilder_h
