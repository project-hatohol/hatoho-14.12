/*
 * Copyright (C) 2014 Project Hatohol
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

#include <SimpleSemaphore.h>
#include <Mutex.h>
#include <Reaper.h>
#include <stack>
#include <cstring>
#include <libsoup/soup.h>
#include "Utils.h"
#include "JSONBuilder.h"
#include "JSONParser.h"
#include "HapProcessCeilometer.h"

using namespace std;
using namespace mlpl;

static const char *MIME_JSON = "application/json";

struct OpenStackEndPoint {
	string publicURL;
};

struct AcquireContext
{
	StringVector  alarmIds;

	static void clear(AcquireContext *ctx)
	{
		ctx->alarmIds.clear();
	}
};

struct HapProcessCeilometer::Impl {
	string osUsername;
	string osPassword;
	string osTenantName;
	string osAuthURL;

	MonitoringServerInfo serverInfo;
	string token;
	OpenStackEndPoint ceilometerEP;
	AcquireContext    acquireCtx;

	Impl(void)
	{
		// Temporary paramters
		osUsername   = "admin";
		osPassword   = "admin";
		osTenantName = "admin";
		osAuthURL    = "http://botctl:35357/v2.0";
	}
};

// ---------------------------------------------------------------------------
// Public methods
// ---------------------------------------------------------------------------
HapProcessCeilometer::HapProcessCeilometer(int argc, char *argv[])
: HapProcessStandard(argc, argv),
  m_impl(new Impl())
{
}

HapProcessCeilometer::~HapProcessCeilometer()
{
}

// ---------------------------------------------------------------------------
// Protected methods
// ---------------------------------------------------------------------------
HatoholError HapProcessCeilometer::updateAuthTokenIfNeeded(void)
{
	if (!m_impl->token.empty())
		return HTERR_OK;

	string url = m_impl->osAuthURL;
	url += "/tokens";
	SoupMessage *msg = soup_message_new(SOUP_METHOD_POST, url.c_str());
	if (!msg) {
		MLPL_ERR("Failed create SoupMessage: URL: %s\n", url.c_str());
		return HTERR_INVALID_URL;
	}
	Reaper<void> msgReaper(msg, g_object_unref);
	soup_message_headers_set_content_type(msg->request_headers,
	                                      MIME_JSON, NULL);
	JSONBuilder builder;
	builder.startObject();
	builder.startObject("auth");
	builder.add("tenantName", m_impl->osTenantName);
	builder.startObject("passwordCredentials");
	builder.add("username", m_impl->osUsername);
	builder.add("password", m_impl->osPassword);
	builder.endObject(); // passwordCredentials
	builder.endObject(); // auth
	builder.endObject();

	string request_body = builder.generate();
	soup_message_body_append(msg->request_body, SOUP_MEMORY_TEMPORARY,
	                         request_body.c_str(), request_body.size());
	SoupSession *session = soup_session_sync_new();
	guint ret = soup_session_send_message(session, msg);
	if (ret != SOUP_STATUS_OK) {
		MLPL_ERR("Failed to connect: (%d) %s, URL: %s\n",
		         ret, soup_status_get_phrase(ret), url.c_str());
		return HTERR_BAD_REST_RESPONSE_KEYSTONE;
	}
	if (!parseReplyToknes(msg)) {
		MLPL_DBG("body: %" G_GOFFSET_FORMAT ", %s\n",
		         msg->response_body->length, msg->response_body->data);
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	}

	return HTERR_OK;
}

bool HapProcessCeilometer::parseReplyToknes(SoupMessage *msg)
{
	JSONParser parser(msg->response_body->data);
	if (parser.hasError()) {
		MLPL_ERR("Failed to parser %s\n", parser.getErrorMessage());
		return false;
	}

	if (!startObject(parser, "access"))
		return false;
	if (!startObject(parser, "token"))
		return false;

	if (!read(parser, "id", m_impl->token))
		return false;

	parser.endObject(); // access
	if (!startObject(parser, "serviceCatalog"))
		return false;

	const unsigned int count = parser.countElements();
	for (unsigned int idx = 0; idx < count; idx++) {
		if (!parserEndpoints(parser, idx))
			return false;
		if (!m_impl->ceilometerEP.publicURL.empty())
			break;
	}

	// check if there's information about the endpoint of ceilometer
	if (m_impl->ceilometerEP.publicURL.empty()) {
		MLPL_ERR("Failed to get an endpoint of ceilometer\n");
		return false;
	}
	return true;
}

bool HapProcessCeilometer::parserEndpoints(JSONParser &parser,
                                    const unsigned int &index)
{
	JSONParser::PositionStack parserRewinder(parser);
	if (! parserRewinder.pushElement(index)) {
		MLPL_ERR("Failed to parse an element, index: %u\n", index);
		return false;
	}

	string name;
	if (!read(parser, "name", name))
		return false;
	if (name != "ceilometer")
		return true;

	if (!parserRewinder.pushObject("endpoints"))
		return false;

	const unsigned int count = parser.countElements();
	for (unsigned int i = 0; i < count; i++) {
		if (!parser.startElement(i)) {
			MLPL_ERR("Failed to parse an element, index: %u\n", i);
			return false;
		}
		bool succeeded = read(parser, "publicURL",
		                      m_impl->ceilometerEP.publicURL);
		parser.endElement();
		if (!succeeded)
			return false;

		// NOTE: Currently we only use the first information even if
		// there're multiple endpoints
		break;
	}

	return true;
}

HatoholError HapProcessCeilometer::getAlarmList(void)
{
	string url = m_impl->ceilometerEP.publicURL;
	url += "/v2/alarms";
	SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
	if (!msg) {
		MLPL_ERR("Failed create SoupMessage: URL: %s\n", url.c_str());
		return HTERR_INVALID_URL;
	}
	Reaper<void> msgReaper(msg, g_object_unref);
	soup_message_headers_set_content_type(msg->request_headers,
	                                      MIME_JSON, NULL);
	soup_message_headers_append(msg->request_headers,
	                            "X-Auth-Token", m_impl->token.c_str());
	// TODO: Add query condition to get updated alarm only.
	SoupSession *session = soup_session_sync_new();
	guint ret = soup_session_send_message(session, msg);
	if (ret != SOUP_STATUS_OK) {
		MLPL_ERR("Failed to connect: (%d) %s, URL: %s\n",
		         ret, soup_status_get_phrase(ret), url.c_str());
		return HTERR_BAD_REST_RESPONSE_CEILOMETER;
	}
	VariableItemTablePtr trigTablePtr;
	HatoholError err = parseReplyGetAlarmList(msg, trigTablePtr);
	if (err != HTERR_OK) {
		MLPL_DBG("body: %" G_GOFFSET_FORMAT ", %s\n",
		         msg->response_body->length, msg->response_body->data);
	}
	sendTable(HAPI_CMD_SEND_UPDATED_TRIGGERS,
	          static_cast<ItemTablePtr>(trigTablePtr));
	return err;
}

TriggerStatusType HapProcessCeilometer::parseAlarmState(const string &state)
{
	if (state == "ok")
		return TRIGGER_STATUS_OK;
	if (state == "insufficient data")
		return TRIGGER_STATUS_UNKNOWN;
	if (state == "alarm")
		return TRIGGER_STATUS_PROBLEM;
	MLPL_ERR("Unknown alarm: %s\n", state.c_str());
	return TRIGGER_STATUS_UNKNOWN;
}

SmartTime HapProcessCeilometer::parseStateTimestamp(
  const string &stateTimestamp)
{
	int year  = 0;
	int month = 0;
	int day   = 0;
	int hour  = 0;
	int min   = 0;
	int sec   = 0;
	int us    = 0;
	const size_t num = sscanf(stateTimestamp.c_str(),
	                          "%04d-%02d-%02dT%02d:%02d:%02d.%06d",
	                          &year, &month, &day, &hour, &min, &sec, &us);
	const size_t NUM_EXPECT_ELEM = 7;
	// We sometimes get the timestamp without the usec part.
	// So we also accept the result with NUM_EXPECT_ELEM-1.
	if (num != NUM_EXPECT_ELEM-1 && num != NUM_EXPECT_ELEM)
		MLPL_ERR("Failed to parser time: %s\n", stateTimestamp.c_str());

	tm tm;
	memset(&tm, 0, sizeof(tm));
	tm.tm_sec  = sec;
	tm.tm_min  = min;
	tm.tm_hour = hour;
	tm.tm_mday = day;
	tm.tm_mon  = month - 1;
	tm.tm_year = year - 1900;
	const timespec ts = {mktime(&tm), us*1000};
	return SmartTime(ts);
}

uint64_t HapProcessCeilometer::generateHashU64(const string &str)
{
	GChecksum *checkSum =  g_checksum_new(G_CHECKSUM_MD5);
	g_checksum_update(checkSum, (const guchar *)str.c_str(), str.size());

	gsize len = 16;
	guint8 buf[len];
	g_checksum_get_digest(checkSum, buf, &len);
	g_checksum_free(checkSum);

	uint64_t csum64;
	memcpy(&csum64, buf, sizeof(uint64_t));
	return csum64;
}

HatoholError HapProcessCeilometer::parseAlarmElement(
  JSONParser &parser, VariableItemTablePtr &tablePtr,
  const unsigned int &index)
{
	JSONParser::PositionStack parserRewinder(parser);
	if (!parserRewinder.pushElement(index)) {
		MLPL_ERR("Failed to parse an element, index: %u\n", index);
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	}

	// trigger ID (alarm_id)
	string alarmId;
	if (!read(parser, "alarm_id", alarmId))
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	// TODO: Fix a structure to save ID.
	// We temporarily generate the 64bit triggerID and host ID from UUID.
	// Strictly speaking, this way is not safe.
	const uint64_t triggerId = generateHashU64(alarmId);

	// status
	string state;
	if (!read(parser, "state", state))
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	const TriggerStatusType status = parseAlarmState(state);

	// severity
	// NOTE: Ceilometer doesn't have a concept of severity.
	// We use one fixed level.
	const TriggerSeverityType severity = TRIGGER_SEVERITY_ERROR;

	// last change time
	string stateTimestamp;
	if (!read(parser, "state_timestamp", stateTimestamp))
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	SmartTime lastChangeTime = parseStateTimestamp(stateTimestamp);

	// TODO: parse HOST ID from data like the following.
	//  /1/threshold_rule/query/0/field "resource"
	//  /1/threshold_rule/query/0/value "eb79c816-8994-4e21-a26c-51d7d19d0e45"
	//  /1/threshold_rule/query/0/op    "eq"
	HostIdType hostId = INAPPLICABLE_HOST_ID;
	string hostName = "No name";

	// brief. We use the alarm name as a brief.
	if (!parserRewinder.pushObject("threshold_rule"))
		return HTERR_FAILED_TO_PARSE_JSON_DATA;

	string meterName;
	if (!read(parser, "meter_name", meterName))
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	string brief = meterName;

	// fill
	// TODO: Define ItemID without ZBX.
	VariableItemGroupPtr grp;
	grp->addNewItem(ITEM_ID_ZBX_TRIGGERS_TRIGGERID,   triggerId);
	grp->addNewItem(ITEM_ID_ZBX_TRIGGERS_VALUE,       status);
	grp->addNewItem(ITEM_ID_ZBX_TRIGGERS_PRIORITY,    severity);
	grp->addNewItem(ITEM_ID_ZBX_TRIGGERS_LASTCHANGE,
	                (int)lastChangeTime.getAsTimespec().tv_sec);
	grp->addNewItem(ITEM_ID_ZBX_TRIGGERS_DESCRIPTION, brief);
	grp->addNewItem(ITEM_ID_ZBX_TRIGGERS_HOSTID,      hostId);
	tablePtr->add(grp);

	// Register the Alarm ID
	m_impl->acquireCtx.alarmIds.push_back(alarmId);

	return HTERR_OK;
}

HatoholError HapProcessCeilometer::parseReplyGetAlarmList(
  SoupMessage *msg, VariableItemTablePtr &tablePtr)
{
	JSONParser parser(msg->response_body->data);
	if (parser.hasError()) {
		MLPL_ERR("Failed to parser %s\n", parser.getErrorMessage());
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	}

	HatoholError err(HTERR_OK);
	const unsigned int count = parser.countElements();
	m_impl->acquireCtx.alarmIds.reserve(count);
	for (unsigned int i = 0; i < count; i++) {
		err = parseAlarmElement(parser, tablePtr, i);
		if (err != HTERR_OK)
			break;
	}
	return err;
}

HatoholError HapProcessCeilometer::getAlarmHistories(void)
{
	HatoholError err(HTERR_OK);
	for (size_t i = 0; i < m_impl->acquireCtx.alarmIds.size(); i++) {
		err = getAlarmHistory(i);
		if (err != HTERR_OK) {
			MLPL_ERR("Failed to get alarm history: %s\n",
			         m_impl->acquireCtx.alarmIds[i].c_str());
		}
	}
	return err;
}

string  HapProcessCeilometer::getHistoryQueryOption(
  const SmartTime &lastTime)
{
	if (!lastTime.hasValidTime())
		return "";

	tm tim;
	const timespec &ts = lastTime.getAsTimespec();
	HATOHOL_ASSERT(
	  gmtime_r(&ts.tv_sec, &tim),
	  "Failed to call gmtime_r(): %s\n", ((string)lastTime).c_str());
	string query = StringUtils::sprintf(
	  "?q.field=timestamp&q.op=gt&q.value="
	  "%04d-%02d-%02dT%02d%%3A%02d%%3A%02d.%06ld",
	  1900+tim.tm_year, tim.tm_mon + 1, tim.tm_mday,
	  tim.tm_hour, tim.tm_min, tim.tm_sec, ts.tv_nsec/1000);
	return query;
}

HatoholError HapProcessCeilometer::getAlarmHistory(const unsigned int &index)
{
	const char *alarmId = m_impl->acquireCtx.alarmIds[index].c_str();
	const SmartTime lastTime = getTimeOfLastEvent(generateHashU64(alarmId));
	string url = StringUtils::sprintf(
	               "%s/v2/alarms/%s/history%s",
	               m_impl->ceilometerEP.publicURL.c_str(),
	               alarmId, getHistoryQueryOption(lastTime).c_str());

	// TODO: extract the commonly used part with getAlarmList()
	SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
	if (!msg) {
		MLPL_ERR("Failed to create SoupMessage: %s\n", url.c_str());
		return HTERR_INVALID_URL;
	}
	Reaper<void> msgReaper(msg, g_object_unref);
	soup_message_headers_set_content_type(msg->request_headers,
	                                      MIME_JSON, NULL);
	soup_message_headers_append(msg->request_headers,
	                            "X-Auth-Token", m_impl->token.c_str());
	SoupSession *session = soup_session_sync_new();
	guint ret = soup_session_send_message(session, msg);
	if (ret != SOUP_STATUS_OK) {
		MLPL_ERR("Failed to connect: (%d) %s, URL: %s\n",
		         ret, soup_status_get_phrase(ret), url.c_str());
		return HTERR_BAD_REST_RESPONSE_CEILOMETER;
	}

	AlarmTimeMap alarmTimeMap;
	HatoholError err = parseReplyGetAlarmHistory(msg, alarmTimeMap);
	if (err != HTERR_OK) {
		MLPL_DBG("body: %" G_GOFFSET_FORMAT ", %s\n",
		         msg->response_body->length, msg->response_body->data);
		return err;
	}

	MLPL_DBG("The numebr of updated alarms: %zd url: %s\n",
	         alarmTimeMap.size(), url.c_str());
	if (alarmTimeMap.empty())
		return HTERR_OK;

	// Build the table
	VariableItemTablePtr eventTablePtr;
	AlarmTimeMapConstIterator it = alarmTimeMap.begin();
	for (; it != alarmTimeMap.end(); ++it) {
		const ItemGroupPtr &historyElement = it->second;
		eventTablePtr->add(historyElement);
	}
	sendTable(HAPI_CMD_SEND_UPDATED_EVENTS,
	          static_cast<ItemTablePtr>(eventTablePtr));
	return HTERR_OK;
}

HatoholError HapProcessCeilometer::parseReplyGetAlarmHistory(
  SoupMessage *msg, AlarmTimeMap &alarmTimeMap)
{
	JSONParser parser(msg->response_body->data);
	if (parser.hasError()) {
		MLPL_ERR("Failed to parser %s\n", parser.getErrorMessage());
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	}

	HatoholError err(HTERR_OK);
	const unsigned int count = parser.countElements();
	for (unsigned int i = 0; i < count; i++) {
		err = parseReplyGetAlarmHistoryElement(parser, alarmTimeMap, i);
		if (err != HTERR_OK)
			break;
	}
	return err;
}

HatoholError HapProcessCeilometer::parseReplyGetAlarmHistoryElement(
  JSONParser &parser, AlarmTimeMap &alarmTimeMap, const unsigned int &index)
{
	JSONParser::PositionStack parserRewinder(parser);
	if (! parserRewinder.pushElement(index)) {
		MLPL_ERR("Failed to parse an element, index: %u\n", index);
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	}

	// Event ID
	string eventIdStr;
	if (!read(parser, "event_id", eventIdStr))
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	// TODO: Fix a structure to save ID.
	// We temporarily generate the 64bit triggerID and host ID from UUID.
	// Strictly speaking, this way is not safe.
	const uint64_t eventId = generateHashU64(eventIdStr);

	// Timestamp
	string timestampStr;
	if (!read(parser, "timestamp", timestampStr))
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	const SmartTime timestamp = parseStateTimestamp(timestampStr);

	// type
	string typeStr;
	if (!read(parser, "type", typeStr))
		return HTERR_FAILED_TO_PARSE_JSON_DATA;

	// Status
	EventType type = EVENT_TYPE_UNKNOWN;
	if (typeStr == "creation" || typeStr == "state transition") {
		string detail;
		if (!read(parser, "detail", detail))
			return HTERR_FAILED_TO_PARSE_JSON_DATA;
		type = parseAlarmHistoryDetail(detail);
	} else {
		MLPL_BUG("Unknown type: %s\n", typeStr.c_str());
	}

	// Trigger ID (alarm ID)
	string alarmIdStr;
	if (!read(parser, "alarm_id", alarmIdStr))
		return HTERR_FAILED_TO_PARSE_JSON_DATA;
	const uint64_t alarmId = generateHashU64(alarmIdStr);

	// Fill table.
	// TODO: Define ItemID without ZBX.
	// TODO: This is originally defined in HatoholDBUtils.cc.
	//       But this Zabbix dependent constant shouldn't be used.
	static const int EVENT_OBJECT_TRIGGER = 0;
	const timespec &ts = timestamp.getAsTimespec();
	VariableItemGroupPtr grp;
	grp->addNewItem(ITEM_ID_ZBX_EVENTS_EVENTID,   eventId);
	grp->addNewItem(ITEM_ID_ZBX_EVENTS_OBJECT,    EVENT_OBJECT_TRIGGER);
	grp->addNewItem(ITEM_ID_ZBX_EVENTS_OBJECTID,  alarmId);
	grp->addNewItem(ITEM_ID_ZBX_EVENTS_CLOCK,     (int)ts.tv_sec);
	grp->addNewItem(ITEM_ID_ZBX_EVENTS_VALUE,     type);
	grp->addNewItem(ITEM_ID_ZBX_EVENTS_NS,        (int)ts.tv_nsec);
	grp->freeze();
	alarmTimeMap.insert(pair<SmartTime, ItemGroupPtr>(timestamp,
	                                                  (ItemGroupPtr)grp));
	return HTERR_OK;
}

EventType HapProcessCeilometer::parseAlarmHistoryDetail(
  const std::string &detail)
{
	JSONParser parser(detail);
	if (parser.hasError()) {
		MLPL_ERR("Failed to parse: %s\n", detail.c_str());
		return EVENT_TYPE_UNKNOWN;
	}

	string state;
	if (!read(parser, "state", state)) {
		MLPL_ERR("Not found 'state': %s\n", detail.c_str());
		return EVENT_TYPE_UNKNOWN;
	}

	if (state == "ok")
		return EVENT_TYPE_GOOD;
	else if (state == "alarm")
		return EVENT_TYPE_BAD;
	else if (state == "insufficient data")
		return EVENT_TYPE_UNKNOWN;
	MLPL_ERR("Unknown state: %s\n", state.c_str());
	return EVENT_TYPE_UNKNOWN;
}

bool HapProcessCeilometer::startObject(
  JSONParser &parser, const string &name)
{
	if (parser.startObject(name))
		return true;
	MLPL_ERR("Not found object: %s\n", name.c_str());
	return false;
}

bool HapProcessCeilometer::read(
  JSONParser &parser, const string &member, string &dest)
{
	if (parser.read(member, dest))
		return true;
	MLPL_ERR("Failed to read: %s\n", member.c_str());
	return false;
}

void HapProcessCeilometer::acquireData(void)
{
	Reaper<AcquireContext>
	  acqCtxCleaner(&m_impl->acquireCtx, AcquireContext::clear);

	MLPL_DBG("acquireData\n");
	updateAuthTokenIfNeeded();
	getAlarmList();      // Trigger
	getAlarmHistories(); // Event
	// TODO: Add get trigger, event, and so on.
}
