/*
 * Copyright (C) 2013-2014 Project Hatohol
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif // HAVE_CONFIG_H

#include <cstring>
#include <Logger.h>
#include <Reaper.h>
#include <MutexLock.h>
#include <SmartTime.h>
#include <AtomicValue.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <semaphore.h>
#include "FaceRest.h"
#include "FaceRestPrivate.h"
#include "JsonBuilderAgent.h"
#include "HatoholException.h"
#include "UnifiedDataStore.h"
#include "DBClientUser.h"
#include "DBClientConfig.h"
#include "SessionManager.h"
#include "CacheServiceDBClient.h"
#include "HatoholArmPluginInterface.h"
#include "HatoholArmPluginGate.h"

using namespace std;
using namespace mlpl;

int FaceRest::API_VERSION = 3;
const char *FaceRest::SESSION_ID_HEADER_NAME = "X-Hatohol-Session";
const int FaceRest::DEFAULT_NUM_WORKERS = 4;

static const guint DEFAULT_PORT = 33194;

const char *FaceRest::pathForTest        = "/test";
const char *FaceRest::pathForLogin       = "/login";
const char *FaceRest::pathForLogout      = "/logout";
const char *FaceRest::pathForGetOverview = "/overview";
const char *FaceRest::pathForServer      = "/server";
const char *FaceRest::pathForServerConnStat = "/server-conn-stat";
const char *FaceRest::pathForGetHost     = "/host";
const char *FaceRest::pathForGetTrigger  = "/trigger";
const char *FaceRest::pathForGetEvent    = "/event";
const char *FaceRest::pathForGetItem     = "/item";
const char *FaceRest::pathForAction      = "/action";
const char *FaceRest::pathForUser        = "/user";
const char *FaceRest::pathForHostgroup   = "/hostgroup";
const char *FaceRest::pathForUserRole    = "/user-role";

static const char *MIME_HTML = "text/html";
static const char *MIME_JSON = "application/json";
static const char *MIME_JAVASCRIPT = "text/javascript";

#define RETURN_IF_NOT_TEST_MODE(ARG) \
do { \
	if (!isTestMode()) { \
		replyError(ARG, HTERR_NOT_TEST_MODE); \
		return; \
	}\
} while(0)

typedef map<string, FormatType> FormatTypeMap;
typedef FormatTypeMap::iterator FormatTypeMapIterator;
static FormatTypeMap g_formatTypeMap;

typedef map<FormatType, const char *> MimeTypeMap;
typedef MimeTypeMap::iterator   MimeTypeMapIterator;
static MimeTypeMap g_mimeTypeMap;

struct FaceRest::PrivateContext {
	struct MainThreadCleaner;
	static bool         testMode;
	static MutexLock    lock;
	static const string pathForUserMe;
	guint               port;
	SoupServer         *soupServer;
	GMainContext       *gMainCtx;
	FaceRestParam      *param;
	AtomicValue<bool>   quitRequest;

	// for async mode
	bool             asyncMode;
	size_t           numPreLoadWorkers;
	set<Worker *>    workers;
	queue<RestJob *> restJobQueue;
	MutexLock        restJobLock;
	sem_t            waitJobSemaphore;

	PrivateContext(FaceRestParam *_param)
	: port(DEFAULT_PORT),
	  soupServer(NULL),
	  gMainCtx(NULL),
	  param(_param),
	  quitRequest(false),
	  asyncMode(true),
	  numPreLoadWorkers(DEFAULT_NUM_WORKERS)
	{
		gMainCtx = g_main_context_new();
		sem_init(&waitJobSemaphore, 0, 0);
	}

	~PrivateContext()
	{
		g_main_context_unref(gMainCtx);
		sem_destroy(&waitJobSemaphore);
	}

	static string initPathForUserMe(void)
	{
		return string(FaceRest::pathForUser) + "/me";
	}

	void pushJob(RestJob *job)
	{
		restJobLock.lock();
		restJobQueue.push(job);
		if (sem_post(&waitJobSemaphore) == -1)
			MLPL_ERR("Failed to call sem_post: %d\n",
				 errno);
		restJobLock.unlock();
	}

	bool waitJob(void)
	{
		if (sem_wait(&waitJobSemaphore) == -1)
			MLPL_ERR("Failed to call sem_wait: %d\n", errno);
		return !quitRequest.get();
	}

	RestJob *popJob(void)
	{
		RestJob *job = NULL;
		restJobLock.lock();
		if (!restJobQueue.empty()) {
			job = restJobQueue.front();
			restJobQueue.pop();
		}
		restJobLock.unlock();
		return job;
	}

	static bool isTestPath(const string &path)
	{
		size_t len = strlen(pathForTest);
		return (strncmp(path.c_str(), pathForTest, len) == 0);
	}
};

bool         FaceRest::PrivateContext::testMode = false;
MutexLock    FaceRest::PrivateContext::lock;
const string FaceRest::PrivateContext::pathForUserMe =
  FaceRest::PrivateContext::initPathForUserMe();

class FaceRest::Worker : public HatoholThreadBase {
public:
	Worker(FaceRest *faceRest)
	: m_faceRest(faceRest)
	{
	}
	virtual ~Worker()
	{
	}

	virtual void waitExit(void) override
	{
		if (!isStarted())
			return;
		HatoholThreadBase::waitExit();
	}

protected:
	virtual gpointer mainThread(HatoholThreadArg *arg)
	{
		RestJob *job;
		MLPL_INFO("start face-rest worker\n");
		while ((job = waitNextJob())) {
			launchHandlerInTryBlock(job);
			finishRestJobIfNeeded(job);
		}
		MLPL_INFO("exited face-rest worker\n");
		return NULL;
	}

private:
	RestJob *waitNextJob(void)
	{
		while (m_faceRest->m_ctx->waitJob()) {
			RestJob *job = m_faceRest->m_ctx->popJob();
			if (job)
				return job;
		}
		return NULL;
	}

	FaceRest *m_faceRest;
};

// ---------------------------------------------------------------------------
// Public methods
// ---------------------------------------------------------------------------
void FaceRest::init(void)
{
	g_formatTypeMap["html"] = FORMAT_HTML;
	g_formatTypeMap["json"] = FORMAT_JSON;
	g_formatTypeMap["jsonp"] = FORMAT_JSONP;

	g_mimeTypeMap[FORMAT_HTML] = MIME_HTML;
	g_mimeTypeMap[FORMAT_JSON] = MIME_JSON;
	g_mimeTypeMap[FORMAT_JSONP] = MIME_JAVASCRIPT;
}

void FaceRest::reset(const CommandLineArg &arg)
{
	bool foundTestMode = false;
	for (size_t i = 0; i < arg.size(); i++) {
		if (arg[i] == "--test-mode")
			foundTestMode = true;
	}

	if (foundTestMode)
		MLPL_INFO("Run as a test mode.\n");
	PrivateContext::testMode = foundTestMode;
}

bool FaceRest::isTestMode(void)
{
	return PrivateContext::testMode;
}

FaceRest::FaceRest(CommandLineArg &cmdArg, FaceRestParam *param)
: m_ctx(NULL)
{
	m_ctx = new PrivateContext(param);

	DBClientConfig dbConfig;
	int port = dbConfig.getFaceRestPort();
	if (port != 0 && Utils::isValidPort(port))
		m_ctx->port = port;

	for (size_t i = 0; i < cmdArg.size(); i++) {
		string &cmd = cmdArg[i];
		if (cmd == "--face-rest-port")
			i = parseCmdArgPort(cmdArg, i);
	}
	MLPL_INFO("started face-rest, port: %d\n", m_ctx->port);
}

FaceRest::~FaceRest()
{
	waitExit();

	MLPL_INFO("FaceRest: stop process: started.\n");
	if (m_ctx->soupServer) {
		SoupSocket *sock = soup_server_get_listener(m_ctx->soupServer);
		soup_socket_disconnect(sock);
		g_object_unref(m_ctx->soupServer);
	}
	MLPL_INFO("FaceRest: stop process: completed.\n");

	if (m_ctx)
		delete m_ctx;
}

void FaceRest::waitExit(void)
{
	if (isStarted()) {
		m_ctx->quitRequest.set(true);

		// To return g_main_context_iteration() in mainThread()
		struct IterAlarm {
			static gboolean task(gpointer data) {
				return G_SOURCE_REMOVE;
			}
		};
		Utils::setGLibIdleEvent(IterAlarm::task, NULL, m_ctx->gMainCtx);
	}

	HatoholThreadBase::waitExit();
}

void FaceRest::setNumberOfPreLoadWorkers(size_t num)
{
	m_ctx->numPreLoadWorkers = num;
}

// ---------------------------------------------------------------------------
// Protected methods
// ---------------------------------------------------------------------------
struct FaceRest::PrivateContext::MainThreadCleaner {
	PrivateContext *ctx;
	bool running;

	MainThreadCleaner(PrivateContext *_ctx)
	: ctx(_ctx),
	  running(false)
	{
	}

	static void callgate(MainThreadCleaner *obj)
	{
		obj->run();
	}

	void run(void)
	{
		if (ctx->soupServer && running)
			soup_server_quit(ctx->soupServer);
	}
};

typedef struct HandlerClosure
{
	FaceRest *m_faceRest;
	RestHandler m_handler;
	HandlerClosure(FaceRest *faceRest, RestHandler handler)
	: m_faceRest(faceRest), m_handler(handler)
	{
	}
} HandlerClosure;

static void deleteHandlerClosure(gpointer data)
{
	HandlerClosure *arg = static_cast<HandlerClosure *>(data);
	delete arg;
}

bool FaceRest::isAsyncMode(void)
{
	return m_ctx->asyncMode;
}

void FaceRest::startWorkers(void)
{
	for (size_t i = 0; i < m_ctx->numPreLoadWorkers; i++) {
		Worker *worker = new Worker(this);
		worker->start();
		m_ctx->workers.insert(worker);
	}
}

void FaceRest::stopWorkers(void)
{
	set<Worker *> &workers = m_ctx->workers;
	set<Worker *>::iterator it;
	for (it = workers.begin(); it != workers.end(); ++it) {
		// to break Worker::waitNextJob()
		sem_post(&m_ctx->waitJobSemaphore);
	}
	for (it = workers.begin(); it != workers.end(); ++it) {
		Worker *worker = *it;
		// destructor will call stop()
		delete worker;
	}
	workers.clear();
}

gpointer FaceRest::mainThread(HatoholThreadArg *arg)
{
	PrivateContext::MainThreadCleaner cleaner(m_ctx);
	Reaper<PrivateContext::MainThreadCleaner>
	   reaper(&cleaner, PrivateContext::MainThreadCleaner::callgate);

	m_ctx->soupServer = soup_server_new(SOUP_SERVER_PORT, m_ctx->port,
	                                    SOUP_SERVER_ASYNC_CONTEXT,
	                                    m_ctx->gMainCtx, NULL);
	if (errno == EADDRINUSE)
		MLPL_ERR("%s", Utils::getUsingPortInfo(m_ctx->port).c_str());
	HATOHOL_ASSERT(m_ctx->soupServer,
	               "failed: soup_server_new: %u, errno: %d\n",
	               m_ctx->port, errno);
	soup_server_add_handler(m_ctx->soupServer, NULL,
	                        handlerDefault, this, NULL);
	soup_server_add_handler(m_ctx->soupServer, "/hello.html",
	                        queueRestJob,
	                        new HandlerClosure(this, &handlerHelloPage),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForTest,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerTest),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForLogin,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerLogin),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForLogout,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerLogout),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForGetOverview,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerGetOverview),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForServer,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerServer),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForServerConnStat,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerServerConnStat),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForGetHost,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerGetHost),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForGetTrigger,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerGetTrigger),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForGetEvent,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerGetEvent),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForGetItem,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerGetItem),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForAction,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerAction),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForHostgroup,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerGetHostgroup),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForUser,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerUser),
	                        deleteHandlerClosure);
	soup_server_add_handler(m_ctx->soupServer, pathForUserRole,
	                        queueRestJob,
	                        new HandlerClosure(this, handlerUserRole),
	                        deleteHandlerClosure);
	if (m_ctx->param)
		m_ctx->param->setupDoneNotifyFunc();
	soup_server_run_async(m_ctx->soupServer);
	cleaner.running = true;

	if (isAsyncMode())
		startWorkers();

	while (!m_ctx->quitRequest.get())
		g_main_context_iteration(m_ctx->gMainCtx, TRUE);

	if (isAsyncMode())
		stopWorkers();

	MLPL_INFO("exited face-rest\n");
	return NULL;
}

SoupServer *FaceRest::getSoupServer(void)
{
	return m_ctx->soupServer;
}

GMainContext *FaceRest::getGMainContext(void)
{
	return m_ctx->gMainCtx;
}

const std::string &FaceRest::getPathForUserMe(void)
{
	return m_ctx->pathForUserMe;
}

size_t FaceRest::parseCmdArgPort(CommandLineArg &cmdArg, size_t idx)
{
	if (idx == cmdArg.size() - 1) {
		MLPL_ERR("needs port number.");
		return idx;
	}

	idx++;
	string &port_str = cmdArg[idx];
	int port = atoi(port_str.c_str());
	if (!Utils::isValidPort(port, false)) {
		MLPL_ERR("invalid port: %s, %d\n", port_str.c_str(), port);
		return idx;
	}

	m_ctx->port = port;
	return idx;
}

void FaceRest::addHatoholError(JsonBuilderAgent &agent,
                               const HatoholError &err)
{
	agent.add("apiVersion", API_VERSION);
	agent.add("errorCode", err.getCode());
	if (err != HTERR_OK && !err.getMessage().empty())
		agent.add("errorMessage", err.getMessage());
	if (!err.getOptionMessage().empty())
		agent.add("optionMessages", err.getOptionMessage());
}

void FaceRest::replyError(RestJob *job,
                          const HatoholErrorCode &errorCode,
                          const string &optionMessage)
{
	HatoholError hatoholError(errorCode, optionMessage);
	replyError(job, hatoholError);
}

void FaceRest::replyError(RestJob *job,
                          const HatoholError &hatoholError)
{
	string message = StringUtils::sprintf("%d", hatoholError.getCode());
	const string &codeName = hatoholError.getCodeName();
	const string &optionMessage = hatoholError.getOptionMessage();

	if (!codeName.empty()) {
		message += ", ";
		message += codeName;
	}
	if (!optionMessage.empty()) {
		message += ": ";
		message += optionMessage;
	}
	MLPL_INFO("reply error: %s\n", message.c_str());

	JsonBuilderAgent agent;
	agent.startObject();
	addHatoholError(agent, hatoholError);
	agent.endObject();
	string response = agent.generate();
	if (!job->jsonpCallbackName.empty())
		response = wrapForJsonp(response, job->jsonpCallbackName);
	soup_message_headers_set_content_type(job->message->response_headers,
	                                      MIME_JSON, NULL);
	soup_message_body_append(job->message->response_body, SOUP_MEMORY_COPY,
	                         response.c_str(), response.size());
	soup_message_set_status(job->message, SOUP_STATUS_OK);

	job->replyIsPrepared = true;
}

string FaceRest::wrapForJsonp(const string &jsonBody,
                              const string &callbackName)
{
	string jsonp = callbackName;
	jsonp += "(";
	jsonp += jsonBody;
	jsonp += ")";
	return jsonp;
}

void FaceRest::replyJsonData(JsonBuilderAgent &agent, RestJob *job)
{
	string response = agent.generate();
	if (!job->jsonpCallbackName.empty())
		response = wrapForJsonp(response, job->jsonpCallbackName);
	soup_message_headers_set_content_type(job->message->response_headers,
	                                      job->mimeType, NULL);
	soup_message_body_append(job->message->response_body, SOUP_MEMORY_COPY,
	                         response.c_str(), response.size());
	soup_message_set_status(job->message, SOUP_STATUS_OK);

	job->replyIsPrepared = true;
}

void FaceRest::parseQueryServerId(GHashTable *query,
                                  ServerIdType &serverId)
{
	serverId = ALL_SERVERS;
	if (!query)
		return;
	gchar *value = (gchar *)g_hash_table_lookup(query, "serverId");
	if (!value)
		return;

	ServerIdType svId;
	if (sscanf(value, "%" FMT_SERVER_ID, &svId) == 1)
		serverId = svId;
	else
		MLPL_INFO("Invalid requested ID: %s\n", value);
}

// handlers
void FaceRest::handlerDefault(SoupServer *server, SoupMessage *msg,
                              const char *path, GHashTable *query,
                              SoupClientContext *client, gpointer user_data)
{
	MLPL_DBG("Default handler: path: %s, method: %s\n",
	         path, msg->method);
	soup_message_set_status(msg, SOUP_STATUS_NOT_FOUND);
}

FaceRest::RestJob::RestJob
  (FaceRest *_faceRest, RestHandler _handler, SoupMessage *_msg,
   const char *_path, GHashTable *_query, SoupClientContext *_client)
: message(_msg), path(_path ? _path : ""), query(_query), client(_client),
  faceRest(_faceRest), handler(_handler), mimeType(NULL),
  userId(INVALID_USER_ID),
  replyIsPrepared(false)
{
	if (query)
		g_hash_table_ref(query);

	// Since life-span of other libsoup's objects should always be longer
	// than this object and shoube be managed by libsoup, we don't
	// inclement reference count of them.
}

FaceRest::RestJob::~RestJob()
{
	if (query)
		g_hash_table_unref(query);
}

SoupServer *FaceRest::RestJob::getSoupServer(void)
{
	return faceRest ? faceRest->getSoupServer() : NULL;
}

GMainContext *FaceRest::RestJob::getGMainContext(void)
{
	return faceRest ? faceRest->getGMainContext() : NULL;
}

bool FaceRest::RestJob::pathIsUserMe(void)
{
	if (!faceRest)
		return false;
	return (path == faceRest->getPathForUserMe());
}

string FaceRest::RestJob::getJsonpCallbackName(void)
{
	if (formatType != FORMAT_JSONP)
		return "";
	gpointer value = g_hash_table_lookup(query, "callback");
	if (!value)
		THROW_HATOHOL_EXCEPTION("Not found parameter: callback");

	const char *callbackName = (const char *)value;
	string errMsg;
	if (!Utils::validateJSMethodName(callbackName, errMsg)) {
		THROW_HATOHOL_EXCEPTION(
		  "Invalid callback name: %s", errMsg.c_str());
	}
	return callbackName;
}

bool FaceRest::RestJob::parseFormatType(void)
{
	formatString.clear();
	if (!query) {
		formatType = FORMAT_JSON;
		return true;
	}

	gchar *format = (gchar *)g_hash_table_lookup(query, "fmt");
	if (!format) {
		formatType = FORMAT_JSON; // default value
		return true;
	}
	formatString = format;

	FormatTypeMapIterator fmtIt = g_formatTypeMap.find(format);
	if (fmtIt == g_formatTypeMap.end())
		return false;
	formatType = fmtIt->second;
	return true;
}

bool FaceRest::RestJob::prepare(void)
{
	const char *_sessionId =
	   soup_message_headers_get_one(message->request_headers,
	                                SESSION_ID_HEADER_NAME);
	sessionId = _sessionId ? _sessionId : "";

	bool notFoundSessionId = true;
	if (sessionId.empty()) {
		if (path == pathForLogin || PrivateContext::isTestPath(path)) {
			userId = INVALID_USER_ID;
			notFoundSessionId = false;
		}
	} else {
		SessionManager *sessionMgr = SessionManager::getInstance();
		SessionPtr session = sessionMgr->getSession(sessionId);
		if (session.hasData()) {
			notFoundSessionId = false;
			userId = session->userId;
		}
	}
	if (notFoundSessionId) {
		replyError(this, HTERR_NOT_FOUND_SESSION_ID);
		return false;
	}
	dataQueryContextPtr =
	  DataQueryContextPtr(new DataQueryContext(userId), false);

	// We expect URIs  whose style are the following.
	//
	// Examples:
	// http://localhost:33194/action
	// http://localhost:33194/action?fmt=json
	// http://localhost:33194/action/2345?fmt=html

	// a format type
	if (!parseFormatType()) {
		REPLY_ERROR(this, HTERR_UNSUPPORTED_FORMAT,
		            "%s", formatString.c_str());
		return false;
	}

	// path elements
	StringUtils::split(pathElements, path, '/');

	// MIME
	MimeTypeMapIterator mimeIt = g_mimeTypeMap.find(formatType);
	HATOHOL_ASSERT(
	  mimeIt != g_mimeTypeMap.end(),
	  "Invalid formatType: %d, %s", formatType, path.c_str());
	mimeType = mimeIt->second;

	// jsonp callback name
	jsonpCallbackName = getJsonpCallbackName();

	return true;
}

void FaceRest::RestJob::pauseResponse(void)
{
	soup_server_pause_message(getSoupServer(), message);
}

struct UnpauseContext {
	SoupServer *server;
	SoupMessage *message;
};

static gboolean idleUnpause(gpointer data)
{
	UnpauseContext *ctx = static_cast<UnpauseContext *>(data);
	soup_server_unpause_message(ctx->server,
				    ctx->message);
	delete ctx;
	return FALSE;
}

void FaceRest::RestJob::unpauseResponse(void)
{
	if (g_main_context_acquire(getGMainContext())) {
		// FaceRest thread
		soup_server_unpause_message(getSoupServer(), message);
		g_main_context_release(getGMainContext());
	} else {
		// Other threads
		UnpauseContext *unpauseContext = new UnpauseContext;
		unpauseContext->server = getSoupServer();
		unpauseContext->message = message;
		soup_add_completion(getGMainContext(), idleUnpause,
				    unpauseContext);
	}
}

string FaceRest::RestJob::getResourceName(int nest)
{
	size_t idx = nest * 2;
	if (pathElements.size() > idx)
		return pathElements[idx];
	return string();
}

string FaceRest::RestJob::getResourceIdString(int nest)
{
	size_t idx = nest * 2 + 1;
	if (pathElements.size() > idx)
		return pathElements[idx];
	return string();
}

uint64_t FaceRest::RestJob::getResourceId(int nest)
{
	size_t idx = nest * 2 + 1;
	if (pathElements.size() <= idx)
		return INVALID_ID;
	uint64_t id = INVALID_ID;
	if (sscanf(pathElements[idx].c_str(), "%" PRIu64, &id) != 1)
		return INVALID_ID;
	return id;
}

static void copyHashTable (gpointer key, gpointer data, gpointer user_data)
{
	GHashTable *dest = static_cast<GHashTable *>(user_data);
	g_hash_table_insert(dest,
			    g_strdup(static_cast<gchar*>(key)),
			    g_strdup(static_cast<gchar*>(data)));
}

void FaceRest::queueRestJob
  (SoupServer *server, SoupMessage *msg, const char *path,
   GHashTable *_query, SoupClientContext *client, gpointer user_data)
{
	GHashTable *query = _query;
	Reaper<GHashTable> postQueryReaper;
	if (StringUtils::casecmp(msg->method, "POST") ||
	    StringUtils::casecmp(msg->method, "PUT")) {
		// The POST request contains query parameters in the body
		// according to application/x-www-form-urlencoded.
		query = soup_form_decode(msg->request_body->data);
	} else {
		GHashTable *dest = g_hash_table_new_full(g_str_hash,
							 g_str_equal,
							 g_free, g_free);
		if (query)
			g_hash_table_foreach(query, copyHashTable, dest);
		query = dest;
	}
	postQueryReaper.set(query, g_hash_table_unref);

	HandlerClosure *closure = static_cast<HandlerClosure *>(user_data);
	FaceRest *face = closure->m_faceRest;
	RestJob *job = new RestJob(face, closure->m_handler,
				   msg, path, query, client);
	if (!job->prepare())
		return;

	job->pauseResponse();

	if (face->isAsyncMode()) {
		face->m_ctx->pushJob(job);
	} else {
		launchHandlerInTryBlock(job);
		finishRestJobIfNeeded(job);
	}
}

void FaceRest::finishRestJobIfNeeded(RestJob *job)
{
	if (job->replyIsPrepared) {
		job->unpauseResponse();
		delete job;
	}
}

void FaceRest::launchHandlerInTryBlock(RestJob *job)
{
	try {
		(*job->handler)(job);
	} catch (const HatoholException &e) {
		REPLY_ERROR(job, HTERR_GOT_EXCEPTION,
		            "%s", e.getFancyMessage().c_str());
	}
}

void FaceRest::handlerHelloPage(RestJob *job)
{
	string response;
	const char *pageTemplate =
	  "<html>"
	  "HATOHOL Server ver. %s"
	  "</html>";
	response = StringUtils::sprintf(pageTemplate, PACKAGE_VERSION);
	soup_message_body_append(job->message->response_body, SOUP_MEMORY_COPY,
	                         response.c_str(), response.size());
	soup_message_set_status(job->message, SOUP_STATUS_OK);
	job->replyIsPrepared = true;
}

static HatoholError
addHostgroupsMap(FaceRest::RestJob *job, JsonBuilderAgent &outputJson,
                 const MonitoringServerInfo &serverInfo,
                 HostgroupInfoList &hostgroupList)
{
	HostgroupsQueryOption option(job->dataQueryContextPtr);
	option.setTargetServerId(serverInfo.id);
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	HatoholError err = dataStore->getHostgroupInfoList(hostgroupList,
	                                                   option);
	if (err != HTERR_OK) {
		MLPL_ERR("Error: %d, user ID: %" FMT_USER_ID ", "
		         "sv ID: %" FMT_SERVER_ID "\n",
		         err.getCode(), job->userId, serverInfo.id);
		return err;
	}

	HostgroupInfoListIterator it = hostgroupList.begin();
	for (; it != hostgroupList.end(); ++it) {
		const HostgroupInfo &hostgroupInfo = *it;
		outputJson.startObject(
		  StringUtils::toString(hostgroupInfo.groupId));
		outputJson.add("name", hostgroupInfo.groupName);
		outputJson.endObject();
	}
	return HTERR_OK;
}

static HatoholError
addHostgroupsMap(FaceRest::RestJob *job, JsonBuilderAgent &outputJson,
                 const MonitoringServerInfo &serverInfo)
{
	HostgroupInfoList hostgroupList;
	return addHostgroupsMap(job, outputJson, serverInfo, hostgroupList);
}

static HatoholError addOverviewEachServer(FaceRest::RestJob *job,
					  JsonBuilderAgent &agent,
					  MonitoringServerInfo &svInfo,
					  bool &serverIsGoodStatus)
{
	HatoholError err;
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	agent.add("serverId", svInfo.id);
	agent.add("serverHostName", svInfo.hostName);
	agent.add("serverIpAddr", svInfo.ipAddress);
	agent.add("serverNickname", svInfo.nickname);

	// TODO: This implementeation is not effective.
	//       We should add a function only to get the number of list.
	HostInfoList hostInfoList;
	HostsQueryOption option(job->dataQueryContextPtr);
	option.setTargetServerId(svInfo.id);
	dataStore->getHostList(hostInfoList, option);
	agent.add("numberOfHosts", hostInfoList.size());

	ItemsQueryOption itemsQueryOption(job->dataQueryContextPtr);
	itemsQueryOption.setTargetServerId(svInfo.id);
	bool fetchItemsSynchronously = true;
	size_t numberOfItems =
	  dataStore->getNumberOfItems(itemsQueryOption,
				      fetchItemsSynchronously);
	agent.add("numberOfItems", numberOfItems);

	TriggerInfoList triggerInfoList;
	TriggersQueryOption triggersQueryOption(job->dataQueryContextPtr);
	triggersQueryOption.setTargetServerId(svInfo.id);
	agent.add("numberOfTriggers",
		  dataStore->getNumberOfTriggers(triggersQueryOption));
	agent.add("numberOfBadHosts",
		  dataStore->getNumberOfBadHosts(triggersQueryOption));
	size_t numBadTriggers = dataStore->getNumberOfBadTriggers(
				  triggersQueryOption, TRIGGER_SEVERITY_ALL);
	agent.add("numberOfBadTriggers", numBadTriggers);

	// TODO: These elements should be fixed
	// after the funtion concerned is added
	agent.add("numberOfUsers", 0);
	agent.add("numberOfOnlineUsers", 0);
	DataQueryOption dataQueryOption(job->userId);
	MonitoringServerStatus serverStatus;
	serverStatus.serverId = svInfo.id;
	err = dataStore->getNumberOfMonitoredItemsPerSecond(
	  dataQueryOption, serverStatus);
	if (err != HTERR_OK)
		return err;
	string nvps = StringUtils::sprintf("%.2f", serverStatus.nvps);
	agent.add("numberOfMonitoredItemsPerSecond", nvps);

	// Hostgroups
	// TODO: We temtatively returns 'No group'. We should fix it
	//       after host group is supported in Hatohol server.
	agent.startObject("hostgroups");
	HostgroupInfoList hostgroupInfoList;
	err = addHostgroupsMap(job, agent, svInfo, hostgroupInfoList);
	if (err != HTERR_OK) {
		HostgroupInfo hgrpInfo;
		hgrpInfo.id = 0;
		hgrpInfo.serverId = svInfo.id;
		hgrpInfo.groupId = ALL_HOST_GROUPS;
		hgrpInfo.groupName = "All host groups";
		hostgroupInfoList.push_back(hgrpInfo);
	}
	agent.endObject();

	// SystemStatus
	agent.startArray("systemStatus");
	HostgroupInfoListConstIterator hostgrpItr = hostgroupInfoList.begin();
	for (; hostgrpItr != hostgroupInfoList.end(); ++ hostgrpItr) {
		const HostgroupIdType hostgroupId = hostgrpItr->groupId;
		for (int severity = 0;
		     severity < NUM_TRIGGER_SEVERITY; severity++) {
			TriggersQueryOption option(job->dataQueryContextPtr);
			option.setTargetServerId(svInfo.id);
			option.setTargetHostgroupId(hostgroupId);
			agent.startObject();
			agent.add("hostgroupId", hostgroupId);
			agent.add("severity", severity);
			agent.add(
			  "numberOfTriggers",
			  dataStore->getNumberOfBadTriggers
			    (option, (TriggerSeverityType)severity));
			agent.endObject();
		}
	}
	agent.endArray();

	// HostStatus
	serverIsGoodStatus = true;
	agent.startArray("hostStatus");
	hostgrpItr = hostgroupInfoList.begin();
	for (; hostgrpItr != hostgroupInfoList.end(); ++ hostgrpItr) {
		const HostgroupIdType hostgroupId = hostgrpItr->groupId;
		TriggersQueryOption option(job->dataQueryContextPtr);
		option.setTargetServerId(svInfo.id);
		option.setTargetHostgroupId(hostgroupId);
		size_t numBadHosts = dataStore->getNumberOfBadHosts(option);
		agent.startObject();
		agent.add("hostgroupId", hostgroupId);
		agent.add("numberOfGoodHosts",
		          dataStore->getNumberOfGoodHosts(option));
		agent.add("numberOfBadHosts", numBadHosts);
		agent.endObject();
		if (numBadHosts > 0)
			serverIsGoodStatus =false;
	}
	agent.endArray();

	return HTERR_OK;
}

static HatoholError addOverview(FaceRest::RestJob *job, JsonBuilderAgent &agent)
{
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	MonitoringServerInfoList monitoringServers;
	ServerQueryOption option(job->dataQueryContextPtr);
	dataStore->getTargetServers(monitoringServers, option);
	MonitoringServerInfoListIterator it = monitoringServers.begin();
	agent.add("numberOfServers", monitoringServers.size());
	agent.startArray("serverStatus");
	size_t numGoodServers = 0, numBadServers = 0;
	for (; it != monitoringServers.end(); ++it) {
		bool serverIsGoodStatus = false;
		agent.startObject();
		HatoholError err = addOverviewEachServer(job, agent, *it, serverIsGoodStatus);
		if (err != HTERR_OK)
			return err;
		agent.endObject();
		if (serverIsGoodStatus)
			numGoodServers++;
		else
			numBadServers++;
	}
	agent.endArray();

	agent.add("numberOfGoodServers", numGoodServers);
	agent.add("numberOfBadServers", numBadServers);

	return HTERR_OK;
}

static void addHosts(FaceRest::RestJob *job, JsonBuilderAgent &agent,
                     const ServerIdType &targetServerId,
                     const HostgroupIdType &targetHostgroupId,
                     const HostIdType &targetHostId)
{
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	HostInfoList hostInfoList;
	HostsQueryOption option(job->dataQueryContextPtr);
	option.setTargetServerId(targetServerId);
	option.setTargetHostgroupId(targetHostgroupId);
	option.setTargetHostId(targetHostId);
	dataStore->getHostList(hostInfoList, option);

	agent.add("numberOfHosts", hostInfoList.size());
	agent.startArray("hosts");
	HostInfoListIterator it = hostInfoList.begin();
	for (; it != hostInfoList.end(); ++it) {
		HostInfo &hostInfo = *it;
		agent.startObject();
		agent.add("id", hostInfo.id);
		agent.add("serverId", hostInfo.serverId);
		agent.add("hostName", hostInfo.hostName);
		agent.endObject();
	}
	agent.endArray();
}

static void addHostsMap(
  FaceRest::RestJob *job, JsonBuilderAgent &agent,
  const MonitoringServerInfo &serverInfo)
{
	HostgroupIdType targetHostgroupId = ALL_HOST_GROUPS;
	char *value = (char *)g_hash_table_lookup(job->query, "hostgroupId");
	if (value)
		sscanf(value, "%" FMT_HOST_GROUP_ID, &targetHostgroupId);

	HostInfoList hostList;
	HostsQueryOption option(job->dataQueryContextPtr);
	option.setTargetServerId(serverInfo.id);
	option.setTargetHostgroupId(targetHostgroupId);
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	dataStore->getHostList(hostList, option);
	HostInfoListIterator it = hostList.begin();
	agent.startObject("hosts");
	for (; it != hostList.end(); ++it) {
		HostInfo &host = *it;
		agent.startObject(StringUtils::toString(host.id));
		agent.add("name", host.hostName);
		agent.endObject();
	}
	agent.endObject();
}

static string getTriggerBrief(
  FaceRest::RestJob *job, const ServerIdType serverId, const TriggerIdType triggerId)
{
	string triggerBrief;
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	TriggerInfoList triggerInfoList;
	TriggersQueryOption triggersQueryOption(job->dataQueryContextPtr);
	triggersQueryOption.setTargetServerId(serverId);
	triggersQueryOption.setTargetId(triggerId);
	dataStore->getTriggerList(triggerInfoList, triggersQueryOption);

	TriggerInfoListIterator it = triggerInfoList.begin();
	TriggerInfo &firstTriggerInfo = *it;
	TriggerIdType firstId = firstTriggerInfo.id;
	for (; it != triggerInfoList.end(); ++it) {
		TriggerInfo &triggerInfo = *it;
		if (firstId == triggerInfo.id) {
			triggerBrief = triggerInfo.brief;
		} else {
			MLPL_WARN("Failed to getTriggerInfo: "
			          "%" FMT_SERVER_ID ", %" FMT_TRIGGER_ID "\n",
			          serverId, triggerId);
		}
	}

	return triggerBrief;
}

static void addTriggersIdBriefHash(
  FaceRest::RestJob *job,
  JsonBuilderAgent &agent, const MonitoringServerInfo &serverInfo,
  TriggerBriefMaps &triggerMaps, bool lookupTriggerBrief = false)
{
	TriggerBriefMaps::iterator server_it = triggerMaps.find(serverInfo.id);
	agent.startObject("triggers");
	ServerIdType serverId = server_it->first;
	TriggerBriefMap &triggers = server_it->second;
	TriggerBriefMap::iterator it = triggers.begin();
	for (; server_it != triggerMaps.end() && it != triggers.end(); ++it) {
		TriggerIdType triggerId = it->first;
		string &triggerBrief = it->second;
		if (lookupTriggerBrief)
			triggerBrief = getTriggerBrief(job,
						       serverId,
						       triggerId);
		agent.startObject(StringUtils::toString(triggerId));
		agent.add("brief", triggerBrief);
		agent.endObject();
	}
	agent.endObject();
}

void FaceRest::addServersMap(
  FaceRest::RestJob *job,
  JsonBuilderAgent &agent,
  TriggerBriefMaps *triggerMaps, bool lookupTriggerBrief)
{
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	MonitoringServerInfoList monitoringServers;
	ServerQueryOption option(job->dataQueryContextPtr);
	dataStore->getTargetServers(monitoringServers, option);

	HatoholError err;
	agent.startObject("servers");
	MonitoringServerInfoListIterator it = monitoringServers.begin();
	for (; it != monitoringServers.end(); ++it) {
		const MonitoringServerInfo &serverInfo = *it;
		agent.startObject(StringUtils::toString(serverInfo.id));
		agent.add("name", serverInfo.hostName);
		agent.add("type", serverInfo.type);
		agent.add("ipAddress", serverInfo.ipAddress);
		addHostsMap(job, agent, serverInfo);
		if (triggerMaps) {
			addTriggersIdBriefHash(job, agent, serverInfo,
					       *triggerMaps,
			                       lookupTriggerBrief);
		}
		agent.startObject("groups");
		// Even if the following function retrun an error,
		// We cannot do anything. The receiver (client) should handle
		// the returned empty or unperfect group information.
		addHostgroupsMap(job, agent, serverInfo);
		agent.endObject(); // "gropus"
		agent.endObject(); // toString(serverInfo.id)
	}
	agent.endObject();
}

void FaceRest::handlerTest(RestJob *job)
{
	JsonBuilderAgent agent;
	agent.startObject();
	addHatoholError(agent, HatoholError(HTERR_OK));
	if (PrivateContext::testMode)
		agent.addTrue("testMode");
	else
		agent.addFalse("testMode");

	if (string(job->path) == "/test" &&
	    string(job->message->method) == "POST")
	{
		agent.startObject("queryData");
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, job->query);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			agent.add(string((const char *)key),
			          string((const char *)value));
		}
		agent.endObject(); // queryData
		agent.endObject(); // top level
		replyJsonData(agent, job);
		return;
	}

	if (string(job->path) == "/test/error") {
		agent.endObject(); // top level
		replyError(job, HTERR_ERROR_TEST);
		return;
	}

	if (string(job->path) == "/test/user" &&
	    string(job->message->method) == "POST")
	{
		RETURN_IF_NOT_TEST_MODE(job);
		UserQueryOption option(USER_ID_SYSTEM);
		HatoholError err = updateOrAddUser(job->query, option);
		if (err != HTERR_OK) {
			replyError(job, err);
			return;
		}
	}

	agent.endObject();
	replyJsonData(agent, job);
}

void FaceRest::handlerLogin(RestJob *job)
{
	gchar *user = (gchar *)g_hash_table_lookup(job->query, "user");
	if (!user) {
		MLPL_INFO("Not found: user\n");
		replyError(job, HTERR_AUTH_FAILED);
		return;
	}
	gchar *password = (gchar *)g_hash_table_lookup(job->query, "password");
	if (!password) {
		MLPL_INFO("Not found: password\n");
		replyError(job, HTERR_AUTH_FAILED);
		return;
	}

	DBClientUser dbUser;
	UserIdType userId = dbUser.getUserId(user, password);
	if (userId == INVALID_USER_ID) {
		MLPL_INFO("Failed to authenticate: Client: %s, User: %s.\n",
			  soup_client_context_get_host(job->client), user);
		replyError(job, HTERR_AUTH_FAILED);
		return;
	}

	SessionManager *sessionMgr = SessionManager::getInstance();
	string sessionId = sessionMgr->create(userId);

	JsonBuilderAgent agent;
	agent.startObject();
	addHatoholError(agent, HatoholError(HTERR_OK));
	agent.add("sessionId", sessionId);
	agent.endObject();

	replyJsonData(agent, job);
}

void FaceRest::handlerLogout(RestJob *job)
{
	SessionManager *sessionMgr = SessionManager::getInstance();
	if (!sessionMgr->remove(job->sessionId)) {
		replyError(job, HTERR_NOT_FOUND_SESSION_ID);
		return;
	}

	JsonBuilderAgent agent;
	agent.startObject();
	addHatoholError(agent, HatoholError(HTERR_OK));
	agent.endObject();

	replyJsonData(agent, job);
}

void FaceRest::handlerGetOverview(RestJob *job)
{
	JsonBuilderAgent agent;
	HatoholError err;
	agent.startObject();
	addHatoholError(agent, HatoholError(HTERR_OK));
	err = addOverview(job, agent);
	if (err != HTERR_OK) {
		replyError(job, err);
		return;
	}
	agent.endObject();

	replyJsonData(agent, job);
}

void FaceRest::handlerGetHost(RestJob *job)
{
	HostsQueryOption option(job->dataQueryContextPtr);
	HatoholError err = parseHostResourceQueryParameter(option, job->query);
	if (err != HTERR_OK) {
		replyError(job, err);
		return;
	}

	JsonBuilderAgent agent;
	agent.startObject();
	addHatoholError(agent, HatoholError(HTERR_OK));
	addHosts(job, agent,
		 option.getTargetServerId(),
		 option.getTargetHostgroupId(),
		 option.getTargetHostId());
	agent.endObject();

	replyJsonData(agent, job);
}

void FaceRest::handlerGetTrigger(RestJob *job)
{
	TriggersQueryOption option(job->dataQueryContextPtr);
	HatoholError err = parseTriggerParameter(option, job->query);
	if (err != HTERR_OK) {
		replyError(job, err);
		return;
	}

	TriggerInfoList triggerList;
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	dataStore->getTriggerList(triggerList, option);

	JsonBuilderAgent agent;
	agent.startObject();
	addHatoholError(agent, HatoholError(HTERR_OK));
	agent.startArray("triggers");
	TriggerInfoListIterator it = triggerList.begin();
	for (; it != triggerList.end(); ++it) {
		TriggerInfo &triggerInfo = *it;
		agent.startObject();
		agent.add("id",       triggerInfo.id);
		agent.add("status",   triggerInfo.status);
		agent.add("severity", triggerInfo.severity);
		agent.add("lastChangeTime",
		          triggerInfo.lastChangeTime.tv_sec);
		agent.add("serverId", triggerInfo.serverId);
		agent.add("hostId",   triggerInfo.hostId);
		agent.add("brief",    triggerInfo.brief);
		agent.endObject();
	}
	agent.endArray();
	agent.add("numberOfTriggers", triggerList.size());
	agent.add("totalNumberOfTriggers",
		  dataStore->getNumberOfTriggers(option));
	addServersMap(job, agent, NULL, false);
	agent.endObject();

	replyJsonData(agent, job);
}

static uint64_t getLastUnifiedEventId(FaceRest::RestJob *job)
{
	EventsQueryOption option(job->dataQueryContextPtr);
	option.setMaximumNumber(1);
	option.setSortType(EventsQueryOption::SORT_UNIFIED_ID,
			   DataQueryOption::SORT_DESCENDING);

	EventInfoList eventList;
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	dataStore->getEventList(eventList, option);

	uint64_t lastUnifiedId = 0;
	if (!eventList.empty()) {
		lastUnifiedId = eventList.begin()->unifiedId;
		eventList.clear();
	}
	return lastUnifiedId;
}

static void addIssue(FaceRest::RestJob *job, JsonBuilderAgent &agent,
		     const IssueInfo &issue)
{
	agent.startObject("issue");
	agent.add("location", issue.location);
	// TODO: remaining properties will be added later
	agent.endObject();
}

void FaceRest::handlerGetEvent(RestJob *job)
{
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();

	EventInfoList eventList;
	EventsQueryOption option(job->dataQueryContextPtr);
	HatoholError err = parseEventParameter(option, job->query);
	if (err != HTERR_OK) {
		replyError(job, err);
		return;
	}

	bool addIssues = dataStore->isIssueSenderActionEnabled();
	IssueInfoVect issueVect;
	if (addIssues) {
		err = dataStore->getEventList(eventList, option, &issueVect);
		HATOHOL_ASSERT(eventList.size() == issueVect.size(),
			       "eventList: %zd, issueVect: %zd\n",
			       eventList.size(), issueVect.size());
	} else {
		err = dataStore->getEventList(eventList, option);
	}
	if (err != HTERR_OK) {
		replyError(job, err);
		return;
	}

	JsonBuilderAgent agent;
	agent.startObject();
	addHatoholError(agent, HatoholError(HTERR_OK));
	// TODO: should use transaction to avoid conflicting with event list
	agent.add("lastUnifiedEventId", getLastUnifiedEventId(job));
	if (addIssues)
		agent.addTrue("haveIssue");
	else
		agent.addFalse("haveIssue");
	agent.startArray("events");
	EventInfoListIterator it = eventList.begin();
	for (size_t i = 0; it != eventList.end(); ++i, ++it) {
		EventInfo &eventInfo = *it;
		agent.startObject();
		agent.add("unifiedId", eventInfo.unifiedId);
		agent.add("serverId",  eventInfo.serverId);
		agent.add("time",      eventInfo.time.tv_sec);
		agent.add("type",      eventInfo.type);
		agent.add("triggerId", eventInfo.triggerId);
		agent.add("status",    eventInfo.status);
		agent.add("severity",  eventInfo.severity);
		agent.add("hostId",    eventInfo.hostId);
		agent.add("brief",     eventInfo.brief);
		if (addIssues)
			addIssue(job, agent, issueVect[i]);
		agent.endObject();
	}
	agent.endArray();
	agent.add("numberOfEvents", eventList.size());
	addServersMap(job, agent, NULL, false);
	agent.endObject();

	replyJsonData(agent, job);
}

struct GetItemClosure : Closure<FaceRest>
{
	struct FaceRest::RestJob *m_restJob;
	GetItemClosure(FaceRest *receiver,
		       callback func,
		       struct FaceRest::RestJob *restJob)
	: Closure<FaceRest>::Closure(receiver, func), m_restJob(restJob)
	{
	}

	virtual ~GetItemClosure()
	{
		delete m_restJob;
	}
};

void FaceRest::replyGetItem(RestJob *job)
{
	ItemsQueryOption option(job->dataQueryContextPtr);
	HatoholError err = parseItemParameter(option, job->query);
	if (err != HTERR_OK) {
		replyError(job, err);
		return;
	}

	ItemInfoList itemList;
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	dataStore->getItemList(itemList, option);

	JsonBuilderAgent agent;
	agent.startObject();
	addHatoholError(agent, HatoholError(HTERR_OK));
	agent.startArray("items");
	ItemInfoListIterator it = itemList.begin();
	for (; it != itemList.end(); ++it) {
		ItemInfo &itemInfo = *it;
		agent.startObject();
		agent.add("id",        itemInfo.id);
		agent.add("serverId",  itemInfo.serverId);
		agent.add("hostId",    itemInfo.hostId);
		agent.add("brief",     itemInfo.brief.c_str());
		agent.add("lastValueTime",
		          itemInfo.lastValueTime.tv_sec);
		agent.add("lastValue", itemInfo.lastValue);
		agent.add("prevValue", itemInfo.prevValue);
		agent.add("itemGroupName", itemInfo.itemGroupName);
		agent.endObject();
	}
	agent.endArray();
	agent.add("numberOfItems", itemList.size());
	agent.add("totalNumberOfItems", dataStore->getNumberOfItems(option));
	addServersMap(job, agent, NULL, false);
	agent.endObject();

	replyJsonData(agent, job);
}

void FaceRest::itemFetchedCallback(ClosureBase *closure)
{
	GetItemClosure *data = dynamic_cast<GetItemClosure*>(closure);
	RestJob *job = data->m_restJob;
	replyGetItem(job);
	job->unpauseResponse();
}

void FaceRest::handlerGetItem(RestJob *job)
{
	ItemsQueryOption option(job->dataQueryContextPtr);
	HatoholError err = parseItemParameter(option, job->query);
	if (err != HTERR_OK) {
		replyError(job, err);
		return;
	}

	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	ServerIdType serverId = option.getTargetServerId();
	FaceRest *face = job->faceRest;
	GetItemClosure *closure =
	  new GetItemClosure(face, &FaceRest::itemFetchedCallback, job);

	// TODO: Fix a crash.
	// There is a bug. When 'closure' is cleared before
	// finishRestJobIfNeeded(RestJob *job) is called, 'job' is deleted
	// in the destructor and becomes a dangling pointer. Then it is used
	// in finishRestJobIfNeeded(). As a result, hatohol crashes.
	// NOTE: Actually, fetching items takes a little time.
	// So it happens rarely.
	bool handled = dataStore->fetchItemsAsync(closure, serverId);
	if (!handled) {
		face->replyGetItem(job);
		// avoid freeing m_restJob because m_restJob will be freed at
		// queueRestJob()
		closure->m_restJob = NULL;
		delete closure;
	}
}

void FaceRest::handlerGetHostgroup(RestJob *job)
{
	HostgroupsQueryOption option(job->dataQueryContextPtr);
	HatoholError err = parseHostResourceQueryParameter(option, job->query);
	if (err != HTERR_OK) {
		replyError(job, err);
		return;
	}

	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();
	HostgroupInfoList hostgroupInfoList;
	err = dataStore->getHostgroupInfoList(hostgroupInfoList, option);

	JsonBuilderAgent agent;
	agent.startObject();
	addHatoholError(agent, err);
	agent.add("numberOfHostgroups", hostgroupInfoList.size());
	agent.startArray("hostgroups");
	HostgroupInfoListIterator it = hostgroupInfoList.begin();
	for (; it != hostgroupInfoList.end(); ++it) {
		HostgroupInfo hostgroupInfo = *it;
		agent.startObject();
		agent.add("id", hostgroupInfo.id);
		agent.add("serverId", hostgroupInfo.serverId);
		agent.add("groupId", hostgroupInfo.groupId);
		agent.add("groupName", hostgroupInfo.groupName.c_str());
		addHostsIsMemberOfGroup(job, agent,
		                        hostgroupInfo.serverId,
		                        hostgroupInfo.groupId);
		agent.endObject();
	}
	agent.endArray();
	agent.endObject();

	replyJsonData(agent, job);
}

void FaceRest::addHostsIsMemberOfGroup(
  RestJob *job, JsonBuilderAgent &agent,
  uint64_t targetServerId, uint64_t targetGroupId)
{
	UnifiedDataStore *dataStore = UnifiedDataStore::getInstance();

	HostgroupElementList hostgroupElementList;
	HostgroupElementQueryOption option(job->dataQueryContextPtr);
	option.setTargetServerId(targetServerId);
	option.setTargetHostgroupId(targetGroupId);
	dataStore->getHostgroupElementList(hostgroupElementList, option);

	agent.startArray("hosts");
	HostgroupElementListIterator it = hostgroupElementList.begin();
	for (; it != hostgroupElementList.end(); ++it) {
		HostgroupElement hostgroupElement = *it;
		agent.add(hostgroupElement.hostId);
	}
	agent.endArray();
}

static HatoholError parseSortTypeFromQuery(
  EventsQueryOption::SortType &sortType, GHashTable *query)
{
	const char *key = "sortType";
	char *value = (char *)g_hash_table_lookup(query, key);
	if (!value)
		return HTERR_NOT_FOUND_PARAMETER;
	if (!strcasecmp(value, "time")) {
		sortType = EventsQueryOption::SORT_TIME;
	} else if (!strcasecmp(value, "unifiedId")) {
		sortType = EventsQueryOption::SORT_UNIFIED_ID;
	} else {
		string optionMessage
		  = StringUtils::sprintf("%s: %s", key, value);
		return HatoholError(HTERR_INVALID_PARAMETER, optionMessage);
	}
	return HatoholError(HTERR_OK);
}

HatoholError FaceRest::parseSortOrderFromQuery(
  DataQueryOption::SortDirection &sortDirection, GHashTable *query)
{
	HatoholError err =
	   getParam<DataQueryOption::SortDirection>(query, "sortOrder", "%d",
						    sortDirection);
	if (err != HTERR_OK)
		return err;
	if (sortDirection != DataQueryOption::SORT_DONT_CARE &&
	    sortDirection != DataQueryOption::SORT_ASCENDING &&
	    sortDirection != DataQueryOption::SORT_DESCENDING) {
		return HatoholError(HTERR_INVALID_PARAMETER,
		                    StringUtils::sprintf("%d", sortDirection));
	}
	return HatoholError(HTERR_OK);
}

HatoholError FaceRest::parseHostResourceQueryParameter(
  HostResourceQueryOption &option, GHashTable *query)
{
	if (!query)
		return HatoholError(HTERR_OK);

	HatoholError err;

	// target server id
	ServerIdType targetServerId = ALL_SERVERS;
	err = getParam<ServerIdType>(query, "serverId",
				     "%" FMT_SERVER_ID,
				     targetServerId);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;
	option.setTargetServerId(targetServerId);

	// target host group id
	HostIdType targetHostgroupId = ALL_HOST_GROUPS;
	err = getParam<HostgroupIdType>(query, "hostgroupId",
					"%" FMT_HOST_GROUP_ID,
					targetHostgroupId);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;
	option.setTargetHostgroupId(targetHostgroupId);

	// target host id
	HostIdType targetHostId = ALL_HOSTS;
	err = getParam<HostIdType>(query, "hostId",
				   "%" FMT_HOST_ID,
				   targetHostId);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;
	option.setTargetHostId(targetHostId);

	// maximum number
	size_t maximumNumber = 0;
	err = getParam<size_t>(query, "maximumNumber", "%zd", maximumNumber);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;
	option.setMaximumNumber(maximumNumber);

	// offset
	uint64_t offset = 0;
	err = getParam<uint64_t>(query, "offset", "%" PRIu64, offset);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;
	option.setOffset(offset);

	return HatoholError(HTERR_OK);
}

HatoholError FaceRest::parseTriggerParameter(TriggersQueryOption &option,
					     GHashTable *query)
{
	if (!query)
		return HatoholError(HTERR_OK);

	HatoholError err;

	// query parameters for HostResourceQueryOption
	err = parseHostResourceQueryParameter(option, query);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;

	// minimum severity
	TriggerSeverityType severity = TRIGGER_SEVERITY_UNKNOWN;
	err = getParam<TriggerSeverityType>(query, "minimumSeverity",
					    "%d", severity);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;
	option.setMinimumSeverity(severity);

	// trigger status
	TriggerStatusType status = TRIGGER_STATUS_ALL;
	err = getParam<TriggerStatusType>(query, "status",
					  "%d", status);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;
	option.setTriggerStatus(status);

	return HatoholError(HTERR_OK);
}

HatoholError FaceRest::parseEventParameter(EventsQueryOption &option,
					   GHashTable *query)
{
	if (!query)
		return HatoholError(HTERR_OK);

	HatoholError err;

	// query parameters for HostResourceQueryOption
	err = parseHostResourceQueryParameter(option, query);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;

	// minimum severity
	TriggerSeverityType severity = TRIGGER_SEVERITY_UNKNOWN;
	err = getParam<TriggerSeverityType>(query, "minimumSeverity",
					    "%d", severity);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;
	option.setMinimumSeverity(severity);

	// trigger status
	TriggerStatusType status = TRIGGER_STATUS_ALL;
	err = getParam<TriggerStatusType>(query, "status",
					  "%d", status);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;
	option.setTriggerStatus(status);

	// sort type
	EventsQueryOption::SortType sortType = EventsQueryOption::SORT_TIME;
	err = parseSortTypeFromQuery(sortType, query);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;

	// sort order
	DataQueryOption::SortDirection sortDirection
	  = DataQueryOption::SORT_DESCENDING;
	err = parseSortOrderFromQuery(sortDirection, query);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;

	option.setSortType(sortType, sortDirection);

	// limit of unifiedId
	uint64_t limitOfUnifiedId = 0;
	err = getParam<uint64_t>(query, "limitOfUnifiedId", "%" PRIu64,
				 limitOfUnifiedId);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;
	option.setLimitOfUnifiedId(limitOfUnifiedId);

	return HatoholError(HTERR_OK);
}

HatoholError FaceRest::parseItemParameter(ItemsQueryOption &option,
					  GHashTable *query)
{
	if (!query)
		return HatoholError(HTERR_OK);

	HatoholError err;

	// query parameters for HostResourceQueryOption
	err = parseHostResourceQueryParameter(option, query);
	if (err != HTERR_OK && err != HTERR_NOT_FOUND_PARAMETER)
		return err;

	// itemGroupName
	const gchar *value = static_cast<const gchar*>(
	  g_hash_table_lookup(query, "itemGroupName"));
	if (value && *value)
		option.setTargetItemGroupName(value);

	return HatoholError(HTERR_OK);
}
