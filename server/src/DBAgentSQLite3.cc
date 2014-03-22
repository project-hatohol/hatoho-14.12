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

#include <cstring>
#include <cstdio>
#include <stdarg.h>
#include <inttypes.h>
#include <gio/gio.h>
#include <MutexLock.h>
#include <Logger.h>
using namespace std;
using namespace mlpl;

#include "DBAgentSQLite3.h"
#include "HatoholException.h"
#include "ConfigManager.h"

const static int TRANSACTION_TIME_OUT_MSEC = 30 * 1000;

#define MAKE_SQL_STATEMENT_FROM_VAARG(LAST_ARG, STR_NAME) \
string STR_NAME; \
{ \
	va_list ap; \
	va_start(ap, fmt); \
	char *_sql = sqlite3_vmprintf(fmt, ap); \
	STR_NAME = _sql; \
	sqlite3_free(_sql); \
	va_end(ap); \
} \

typedef map<DBDomainId, string>     DBDomainIdPathMap;
typedef DBDomainIdPathMap::iterator DBDomainIdPathMapIterator;

struct DBAgentSQLite3::PrivateContext {
	static MutexLock         mutex;
	static DBDomainIdPathMap domainIdPathMap;

	string        dbPath;
	sqlite3      *db;

	// methods
	PrivateContext(void)
	: db(NULL)
	{
	}

	static void lock(void)
	{
		mutex.lock();
	}

	static void unlock(void)
	{
		mutex.unlock();
	}

	static bool isDBPathDefined(DBDomainId domainId)
	{
		lock();
		DBDomainIdPathMapIterator it = domainIdPathMap.find(domainId);
		bool defined = (it != domainIdPathMap.end());
		unlock();
		return defined;
	}
};

MutexLock         DBAgentSQLite3::PrivateContext::mutex;
DBDomainIdPathMap DBAgentSQLite3::PrivateContext::domainIdPathMap;

// ---------------------------------------------------------------------------
// Public methods
// ---------------------------------------------------------------------------
void DBAgentSQLite3::init(void)
{
	// Currently ConfigManger doesn't have init(), so calling
	// the following functions and getDefaultDBPath() that use
	// ConfigManager is no problem.
	// However, if we add ConfigManager::init() in the future,
	// The order of ConfigManager::init() and DBAgentSQLite3::init() has
	// be well considered.
	ConfigManager *configMgr = ConfigManager::getInstance();
	const string &dbDirectory = configMgr->getDatabaseDirectory();
	checkDBPath(dbDirectory);

	string dbPath = getDefaultDBPath(DEFAULT_DB_DOMAIN_ID);
	defineDBPath(DEFAULT_DB_DOMAIN_ID, dbPath);
}

void DBAgentSQLite3::reset(void)
{
	PrivateContext::domainIdPathMap.clear();
}

bool DBAgentSQLite3::defineDBPath(DBDomainId domainId, const string &path,
                                  bool allowOverwrite)
{
	bool ret = true;
	PrivateContext::lock();
	DBDomainIdPathMapIterator it =
	  PrivateContext::domainIdPathMap.find(domainId);
	if (it != PrivateContext::domainIdPathMap.end()) {
		if (allowOverwrite)
			it->second = path;
		else
			ret = false;
		PrivateContext::unlock();
		return ret;
	}

	pair<DBDomainIdPathMapIterator, bool> result =
	  PrivateContext::domainIdPathMap.insert
	    (pair<DBDomainId, string>(domainId, path));
	PrivateContext::unlock();

	HATOHOL_ASSERT(result.second,
	  "Failed to insert. Probably domain id (%u) is duplicated", domainId);
	return ret;
}

string &DBAgentSQLite3::getDBPath(DBDomainId domainId)
{
	string dbPath;
	PrivateContext::lock();
	DBDomainIdPathMapIterator it =
	   PrivateContext::domainIdPathMap.find(domainId);
	if (it == PrivateContext::domainIdPathMap.end()) {
		string path = getDefaultDBPath(domainId);
		pair<DBDomainIdPathMapIterator, bool> result =
		  PrivateContext::domainIdPathMap.insert
		    (pair<DBDomainId,string>(domainId, path));
		it = result.first;
	}
	PrivateContext::unlock();
	return it->second;
}

DBAgentSQLite3::DBAgentSQLite3(const string &dbName,
                               DBDomainId domainId, bool skipSetup)
: DBAgent(domainId, skipSetup),
  m_ctx(NULL)
{
	m_ctx = new PrivateContext();
	if (!dbName.empty() && !PrivateContext::isDBPathDefined(domainId)) {
		string dbPath = makeDBPathFromName(dbName);
		const bool allowOverwrite = false;
		defineDBPath(domainId, dbPath, allowOverwrite);
	}
	m_ctx->dbPath = getDBPath(domainId);
	openDatabase();
}

DBAgentSQLite3::~DBAgentSQLite3()
{
	if (!m_ctx)
		return;

	if (m_ctx->db) {
		int result = sqlite3_close(m_ctx->db);
		if (result != SQLITE_OK) {
			// Should we throw an exception ?
			MLPL_ERR("Failed to close sqlite: %d\n", result);
		}
	}

	delete m_ctx;
}

void DBAgentSQLite3::getIndexes(vector<IndexStruct> &indexStructVect,
                                const string &tableName)
{
	string query = StringUtils::sprintf(
	  "SELECT name,tbl_name,sql FROM sqlite_master "
	  "WHERE type='index' and tbl_name='%s';",
	  tableName.c_str());

	sqlite3_stmt *stmt;
	int result;
	result = sqlite3_prepare(m_ctx->db, query.c_str(), query.size(),
	                         &stmt, NULL);
	if (result != SQLITE_OK) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION(
		  "Failed to call sqlite3_prepare(): %d: %s",
		  result, query.c_str());
	}
	sqlite3_reset(stmt);
	while ((result = sqlite3_step(stmt)) == SQLITE_ROW) {
		size_t idx = 0;
		IndexStruct idxStruct;
		idxStruct.name =
		  (const char *)sqlite3_column_text(stmt, idx++);
		idxStruct.tableName =
		  (const char *)sqlite3_column_text(stmt, idx++);
		idxStruct.sql =
		  (const char *)sqlite3_column_text(stmt, idx++);
		indexStructVect.push_back(idxStruct);
	}
	sqlite3_finalize(stmt);
}

bool DBAgentSQLite3::isTableExisting(const string &tableName)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	return isTableExisting(m_ctx->db, tableName);
}

bool DBAgentSQLite3::isRecordExisting(const string &tableName,
                                      const string &condition)
{
	int result;
	sqlite3_stmt *stmt;
	string query = StringUtils::sprintf(
	                 "SELECT * FROM %s WHERE %s",
	                 tableName.c_str(), condition.c_str());
	result = sqlite3_prepare(m_ctx->db, query.c_str(), query.size(),
	                         &stmt, NULL);
	if (result != SQLITE_OK) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION(
		  "Failed to call sqlite3_prepare(): %d: %s",
		  result, query.c_str());
	}
	sqlite3_reset(stmt);
	bool found = false;
	while ((result = sqlite3_step(stmt)) == SQLITE_ROW) {
		found = true;
		break;
	}
	sqlite3_finalize(stmt);
	if (result != SQLITE_ROW && result != SQLITE_DONE) {
		THROW_HATOHOL_EXCEPTION("Failed to call sqlite3_step(): %d",
		                      result);
	}
	return found;
}

void DBAgentSQLite3::begin(void)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	_execSql(m_ctx->db, "BEGIN IMMEDIATE");
}

void DBAgentSQLite3::commit(void)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	_execSql(m_ctx->db, "COMMIT");
}

void DBAgentSQLite3::rollback(void)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	_execSql(m_ctx->db, "ROLLBACK");
}

void DBAgentSQLite3::execSql(const string &sql)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	_execSql(m_ctx->db, sql);
}

void DBAgentSQLite3::createTable(const TableProfile &tableProfile)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	createTable(m_ctx->db, tableProfile);
}

void DBAgentSQLite3::insert(const DBAgent::InsertArg &insertArg)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	insert(m_ctx->db, insertArg);
}

void DBAgentSQLite3::update(const UpdateArg &updateArg)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	update(m_ctx->db, updateArg);
}

void DBAgentSQLite3::select(const SelectArg &selectArg)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	select(m_ctx->db, selectArg);
}

void DBAgentSQLite3::select(const SelectExArg &selectExArg)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	select(m_ctx->db, selectExArg);
}

void DBAgentSQLite3::deleteRows(const DeleteArg &deleteArg)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	deleteRows(m_ctx->db, deleteArg);
}

uint64_t DBAgentSQLite3::getLastInsertId(void)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	return getLastInsertId(m_ctx->db);
}

uint64_t DBAgentSQLite3::getNumberOfAffectedRows(void)
{
	HATOHOL_ASSERT(m_ctx->db, "m_ctx->db is NULL");
	return getNumberOfAffectedRows(m_ctx->db);
}

// ---------------------------------------------------------------------------
// Protected methods
// ---------------------------------------------------------------------------
string DBAgentSQLite3::makeDBPathFromName(const string &name)
{
	ConfigManager *configMgr = ConfigManager::getInstance();
	const string &dbDirectory = configMgr->getDatabaseDirectory();
	string dbPath =
	  StringUtils::sprintf("%s/%s.db", dbDirectory.c_str(), name.c_str());
	return dbPath;
}

string DBAgentSQLite3::getDefaultDBPath(DBDomainId domainId)
{
	string name = StringUtils::sprintf("DBAgentSQLite3-%d", domainId);
	return makeDBPathFromName(name);
}

void DBAgentSQLite3::checkDBPath(const string &dbPath)
{
	GFile *gfile = g_file_new_for_path(dbPath.c_str());

	// check if the specified path is directory
	GFileType type =
	   g_file_query_file_type(gfile, G_FILE_QUERY_INFO_NONE, NULL);
	if (type != G_FILE_TYPE_UNKNOWN && type != G_FILE_TYPE_DIRECTORY) {
		g_object_unref(gfile);
		THROW_HATOHOL_EXCEPTION("Specified dir is not directory:%s\n",
		                      dbPath.c_str());
	}

	// try to the directory if it doesn't exist
	if (type == G_FILE_TYPE_UNKNOWN) {
		GError *error = NULL;
		gboolean successed =
		  g_file_make_directory_with_parents(gfile, NULL, &error);
		if (!successed) {
			string msg = error->message;
			g_error_free(error);
			g_object_unref(gfile);
			THROW_HATOHOL_EXCEPTION(
			  "Failed to create dir. for DB: %s: %s\n",
			  dbPath.c_str(), msg.c_str());
		}
	}

	// Should we check if we can write on the directory ?

	g_object_unref(gfile);
}

sqlite3 *DBAgentSQLite3::openDatabase(const string &dbPath)
{
	sqlite3 *db = NULL;
	int result = sqlite3_open_v2(dbPath.c_str(), &db,
	                             SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,
	                             NULL);
	if (result != SQLITE_OK) {
		THROW_HATOHOL_EXCEPTION("Failed to open sqlite: %d, %s",
		                      result, dbPath.c_str());
	}
	sqlite3_busy_timeout(db, TRANSACTION_TIME_OUT_MSEC);
	return db;
}

void DBAgentSQLite3::_execSql(sqlite3 *db, const string &sql)
{
	char *errmsg;
	int result = sqlite3_exec(db, sql.c_str(), NULL, NULL, &errmsg);
	if (result != SQLITE_OK) {
		string err = errmsg;
		sqlite3_free(errmsg);
		THROW_HATOHOL_EXCEPTION("Failed to exec: %d, %s, %s",
		                      result, err.c_str(), sql.c_str());
	}
}

void DBAgentSQLite3::execSql(sqlite3 *db, const char *fmt, ...)
{
	// make a query string
	MAKE_SQL_STATEMENT_FROM_VAARG(fmt, sql);

	// execute the query
	_execSql(db, sql);
}

bool DBAgentSQLite3::isTableExisting(sqlite3 *db,
                                     const string &tableName)
{
	int result;
	sqlite3_stmt *stmt;
	const char *query = "SELECT COUNT(*) FROM sqlite_master "
	                    "WHERE type='table' AND name=?";
	result = sqlite3_prepare(db, query, strlen(query), &stmt, NULL);
	if (result != SQLITE_OK) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION("Failed to call sqlite3_prepare(): %d",
		                      result);
	}

	sqlite3_reset(stmt);
	result = sqlite3_bind_text(stmt, 1, tableName.c_str(),
	                           -1, SQLITE_STATIC);
	if (result != SQLITE_OK) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION("Failed to call sqlite3_bind(): %d",
		                      result);
	}
	
	int count = 0;
	while ((result = sqlite3_step(stmt)) == SQLITE_ROW) {
		count = sqlite3_column_int(stmt, 0);
	}
	if (result != SQLITE_DONE) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION("Failed to call sqlite3_step(): %d",
		                      result);
	}
	sqlite3_finalize(stmt);
	return count > 0;
}

struct KeyColumnInfo {
	bool   isUnique;
	size_t columnIndex;
};

void DBAgentSQLite3::createTable(sqlite3 *db, const TableProfile &tableProfile)
{
	vector<size_t> multipleKeyColumnIndexVector;

	vector<KeyColumnInfo> keyColumnInfoVector;

	// make a SQL statement
	string sql = "CREATE TABLE ";
	sql += tableProfile.name;
	sql += "(";
	for (size_t i = 0; i < tableProfile.numColumns; i++) {
		const ColumnDef &columnDef = tableProfile.columnDefs[i];

		// set type
		sql += columnDef.columnName;
		sql += " ";
		switch (columnDef.type) {
		case SQL_COLUMN_TYPE_INT:
		case SQL_COLUMN_TYPE_BIGUINT:
			sql += "INTEGER";
			break;
		case SQL_COLUMN_TYPE_VARCHAR:
		case SQL_COLUMN_TYPE_CHAR:
		case SQL_COLUMN_TYPE_TEXT:
			sql += "TEXT";
			break;
		case SQL_COLUMN_TYPE_DOUBLE:
			sql += "REAL";
			break;
		default:
			HATOHOL_ASSERT(true, "Unknown column type: %d (%s)",
			             columnDef.type, columnDef.columnName);
		}
		sql += " ";

		// set key
		switch (columnDef.keyType) {
		case SQL_KEY_PRI:
			sql += "PRIMARY KEY";
			if (columnDef.flags & SQL_COLUMN_FLAG_AUTO_INC)
				sql += " AUTOINCREMENT";
			break;
		case SQL_KEY_MUL:
			multipleKeyColumnIndexVector.push_back(i);
			break;
		case SQL_KEY_UNI:
		case SQL_KEY_IDX:
			KeyColumnInfo keyInfo;
			keyInfo.isUnique = (columnDef.keyType == SQL_KEY_UNI);
			keyInfo.columnIndex = i;
			keyColumnInfoVector.push_back(keyInfo);
			break;
		case SQL_KEY_NONE:
			break;
		default:
			HATOHOL_ASSERT(true, "Unknown column type: %d (%s)",
			             columnDef.keyType, columnDef.columnName);
		}

		if (i < tableProfile.numColumns - 1)
			sql += ",";
	}
	sql += ")";

	// exectute the SQL statement
	char *errmsg;
	int result = sqlite3_exec(db, sql.c_str(), NULL, NULL, &errmsg);
	if (result != SQLITE_OK) {
		string err = errmsg;
		sqlite3_free(errmsg);
		THROW_HATOHOL_EXCEPTION("Failed to exec: %d, %s, %s",
		                      result, err.c_str(), sql.c_str());
	}

	// for single-column indexes
	for (size_t i = 0; i < keyColumnInfoVector.size(); i++) {
		const KeyColumnInfo &keyInfo = keyColumnInfoVector[i];
		const ColumnDef &columnDef =
		  tableProfile.columnDefs[keyInfo.columnIndex];
		string indexName = StringUtils::sprintf(
		  "index_%s_%s", tableProfile.name, columnDef.columnName);
		vector<size_t> columnIndexVector;
		columnIndexVector.push_back(keyInfo.columnIndex);
		createIndexIfNotExistsEach(
		  db, tableProfile, indexName,
		  columnIndexVector, keyInfo.isUnique);
	}

	// for multi-column indexes
	if (!multipleKeyColumnIndexVector.empty()) {
		bool isUniqueKey = false;
		string indexName = StringUtils::sprintf("mul_index_%s",
		                                        tableProfile.name);
		createIndexIfNotExistsEach(
		  db, tableProfile, indexName,
		  multipleKeyColumnIndexVector, isUniqueKey);
	}
}

void DBAgentSQLite3::insert(sqlite3 *db, const DBAgent::InsertArg &insertArg)
{
	size_t numColumns = insertArg.row->getNumberOfItems();
	HATOHOL_ASSERT(numColumns == insertArg.tableProfile.numColumns,
	               "Invalid number of colums: %zd, %zd",
	               numColumns, insertArg.tableProfile.numColumns);

	// make a SQL statement
	string sql = "INSERT INTO ";
	sql += insertArg.tableProfile.name;
	sql += " VALUES (";
	for (size_t i = 0; i < numColumns; i++) {
		if (i > 0)
			sql += ",";
		const ColumnDef &columnDef =
		  insertArg.tableProfile.columnDefs[i];
		const ItemData *itemData = insertArg.row->getItemAt(i);
		string valueStr;
		if (itemData->isNull()) {
			valueStr = "NULL";
		} else {
			valueStr = getColumnValueString(&columnDef, itemData);
			if (columnDef.flags & SQL_COLUMN_FLAG_AUTO_INC) {
				// Converting 0 to NULL makes the behavior
				// compatible with DBAgentMySQL.
				if (valueStr == "0")
					valueStr = "NULL";
			}
		}
		sql += valueStr;
	}
	sql += ")";

	// exectute the SQL statement
	char *errmsg;
	int result = sqlite3_exec(db, sql.c_str(), NULL, NULL, &errmsg);
	if (result != SQLITE_OK) {
		string err = errmsg;
		sqlite3_free(errmsg);
		THROW_HATOHOL_EXCEPTION("Failed to exec: %d, %s, %s",
		                      result, err.c_str(), sql.c_str());
	}
}

void DBAgentSQLite3::update(sqlite3 *db, const UpdateArg &updateArg)
{
	string sql = makeUpdateStatement(updateArg);

	// exectute the SQL statement
	char *errmsg;
	int result = sqlite3_exec(db, sql.c_str(), NULL, NULL, &errmsg);
	if (result != SQLITE_OK) {
		string err = errmsg;
		sqlite3_free(errmsg);
		THROW_HATOHOL_EXCEPTION("Failed to exec: %d, %s, %s",
		                      result, err.c_str(), sql.c_str());
	}
}

void DBAgentSQLite3::select(sqlite3 *db, const SelectArg &selectArg)
{
	string sql = makeSelectStatement(selectArg);

	// exectute
	int result;
	sqlite3_stmt *stmt;
	result = sqlite3_prepare(db, sql.c_str(), sql.size(), &stmt, NULL);
	if (result != SQLITE_OK) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION(
		  "Failed to call sqlite3_prepare(): %d, %s",
		  result, sql.c_str());
	}

	sqlite3_reset(stmt);
	if (result != SQLITE_OK) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION("Failed to call sqlite3_bind(): %d",
		                      result);
	}
	
	VariableItemTablePtr dataTable;
	while ((result = sqlite3_step(stmt)) == SQLITE_ROW)
		selectGetValuesIteration(selectArg, stmt, dataTable);
	selectArg.dataTable = dataTable;
	if (result != SQLITE_DONE) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION("Failed to call sqlite3_step(): %d",
		                      result);
	}
	sqlite3_finalize(stmt);
}

void DBAgentSQLite3::select(sqlite3 *db, const SelectExArg &selectExArg)
{
	string sql = makeSelectStatement(selectExArg);

	// exectute
	int result;
	sqlite3_stmt *stmt;
	result = sqlite3_prepare(db, sql.c_str(), sql.size(), &stmt, NULL);
	if (result != SQLITE_OK) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION(
		  "Failed to call sqlite3_prepare(): %d, %s",
		  result, sql.c_str());
	}

	sqlite3_reset(stmt);
	if (result != SQLITE_OK) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION("Failed to call sqlite3_bind(): %d",
		                      result);
	}
	size_t numColumns = selectExArg.statements.size();
	VariableItemTablePtr dataTable;
	while ((result = sqlite3_step(stmt)) == SQLITE_ROW) {
		VariableItemGroupPtr itemGroup;
		for (size_t index = 0; index < numColumns; index++) {
			ItemDataPtr itemDataPtr =
			  getValue(stmt, index, selectExArg.columnTypes[index]);
			itemGroup->add(itemDataPtr);
		}
		dataTable->add(itemGroup);
	}
	selectExArg.dataTable = dataTable;
	if (result != SQLITE_DONE) {
		sqlite3_finalize(stmt);
		THROW_HATOHOL_EXCEPTION("Failed to call sqlite3_step(): %d",
		                      result);
	}
	sqlite3_finalize(stmt);

	// check the result
	size_t numTableRows = selectExArg.dataTable->getNumberOfRows();
	size_t numTableColumns = selectExArg.dataTable->getNumberOfColumns();
	HATOHOL_ASSERT((numTableRows == 0) ||
	             ((numTableRows > 0) && (numTableColumns == numColumns)),
	             "Sanity check error: numTableRows: %zd, numTableColumns: "
	             "%zd, numColumns: %zd",
	             numTableRows, numTableColumns, numColumns);
}

void DBAgentSQLite3::deleteRows(sqlite3 *db, const DeleteArg &deleteArg)
{
	string sql = makeDeleteStatement(deleteArg);
	_execSql(db, sql.c_str());
}

void DBAgentSQLite3::addColumns(const AddColumnsArg &addColumnsArg)
{
	MLPL_BUG("Not implemented: %s\n", __PRETTY_FUNCTION__);
}

void DBAgentSQLite3::selectGetValuesIteration(const SelectArg &selectArg,
                                              sqlite3_stmt *stmt,
                                              VariableItemTablePtr &dataTable)
{
	VariableItemGroupPtr itemGroup;
	for (size_t i = 0; i < selectArg.columnIndexes.size(); i++) {
		size_t idx = selectArg.columnIndexes[i];
		const ColumnDef &columnDef =
		  selectArg.tableProfile.columnDefs[idx];
		itemGroup->add(getValue(stmt, i, columnDef.type));
	}
	dataTable->add(itemGroup);
}

uint64_t DBAgentSQLite3::getLastInsertId(sqlite3 *db)
{
	return sqlite3_last_insert_rowid(db);
}

uint64_t DBAgentSQLite3::getNumberOfAffectedRows(sqlite3 *db)
{
	return sqlite3_changes(db);
}

void DBAgentSQLite3::fixupIndexes(
  const TableProfile &tableProfile, const IndexDef *indexDefArray)
{
	typedef map<string, IndexStruct *>  IndexSqlStructMap;
	typedef IndexSqlStructMap::iterator IndexSqlStructMapIterator;

	// Gather existing indexes
	vector<IndexStruct> indexStructVect;
	getIndexes(indexStructVect, tableProfile.name);
	IndexSqlStructMap indexSqlStructMap;
	for (size_t i = 0; i < indexStructVect.size(); i++) {
		IndexStruct &idxStruct = indexStructVect[i];
		indexSqlStructMap[idxStruct.sql] = &idxStruct;
	}

	// Create needed indexes with ColumnDef
	// TODO: move the needed code from createTable()

	// Create needed indexes with IndexesDef
	const IndexDef *indexDefPtr = indexDefArray;
	for (; indexDefPtr->name; indexDefPtr++) {
		const string sql = makeCreateIndexStatement(*indexDefPtr);
		IndexSqlStructMapIterator it = indexSqlStructMap.find(sql);
		if (it != indexSqlStructMap.end()) {
			indexSqlStructMap.erase(it);
			continue;
		}
		createIndex(*indexDefPtr);
	}

	// drop remaining (unnecessary) indexes
	/* TODO: Comment out after the block to create indexes with ColumnDef.
	while (!indexSqlStructMap.empty()) {
		IndexSqlStructMapIterator firstElem = indexSqlStructMap.begin();
		const IndexStruct &idxStruct = *firstElem->second;
		dropIndex(idxStruct.name, idxStruct.tableName);
		indexSqlStructMap.erase(firstElem);
	}*/
}

ItemDataPtr DBAgentSQLite3::getValue(sqlite3_stmt *stmt,
                                     size_t index, SQLColumnType columnType)
{
	sqlite3_int64 int64val;
	const char *str;
	ItemData *itemData;
	switch (columnType) {
	case SQL_COLUMN_TYPE_INT:
		itemData = new ItemInt(sqlite3_column_int(stmt, index));
		break;

	case SQL_COLUMN_TYPE_BIGUINT:
		int64val = sqlite3_column_int64(stmt, index);
		itemData = new ItemUint64(int64val);
		break;

	case SQL_COLUMN_TYPE_VARCHAR:
	case SQL_COLUMN_TYPE_CHAR:
	case SQL_COLUMN_TYPE_TEXT:
		str = (const char *)sqlite3_column_text(stmt, index);
		if (!str)
			str = "";
		itemData = new ItemString(str);
		break;

	case SQL_COLUMN_TYPE_DOUBLE:
		itemData = new ItemDouble(sqlite3_column_double(stmt, index));
		break;

	case SQL_COLUMN_TYPE_DATETIME:
		itemData = new ItemInt(sqlite3_column_int(stmt, index));
		break;

	default:
		HATOHOL_ASSERT(false, "Unknown column type: %d", columnType);
	}

	// check null
	if (sqlite3_column_type(stmt, index) == SQLITE_NULL)
		itemData->setNull();

	return ItemDataPtr(itemData, false);
}

void DBAgentSQLite3::createIndexIfNotExistsEach(
  sqlite3 *db, const TableProfile &tableProfile, const string &indexName,
  const vector<size_t> &targetIndexes, const bool &isUniqueKey)
{
	HATOHOL_ASSERT(!targetIndexes.empty(), "target indexes vector is empty.");

	// make an SQL statement
	string sql = "CREATE ";
	if (isUniqueKey)
		sql += "UNIQUE ";
	sql += "INDEX IF NOT EXISTS ";
	sql += indexName;
	sql += " ON ";
	sql += tableProfile.name;
	sql += "(";
	for (size_t i = 0; i < targetIndexes.size(); i++) {
		const size_t targetIdx = targetIndexes[i];
		const ColumnDef &columnDef = tableProfile.columnDefs[targetIdx];
		sql += columnDef.columnName;
		if (i < targetIndexes.size() - 1)
			sql += ",";
	}
	sql += ")";

	// execute the SQL statement
	char *errmsg;
	int result = sqlite3_exec(db, sql.c_str(), NULL, NULL, &errmsg);
	if (result != SQLITE_OK) {
		string err = errmsg;
		sqlite3_free(errmsg);
		THROW_HATOHOL_EXCEPTION("Failed to exec: %d, %s, %s",
		                      result, err.c_str(), sql.c_str());
	}
}

string DBAgentSQLite3::makeCreateIndexStatement(const IndexDef &indexDef)
{
	string sql = StringUtils::sprintf(
	  "CREATE %sINDEX %s ON %s(",
	  indexDef.isUnique ? "UNIQUE " : "",
	  indexDef.name, indexDef.tableProfile.name);

	const int *columnIdxPtr = indexDef.columnIndexes;
	while (true) {
		const ColumnDef &columnDef =
		  indexDef.tableProfile.columnDefs[*columnIdxPtr];
		sql += columnDef.columnName;

		// get the next column index and check if it's the end.
		columnIdxPtr++;
		if (*columnIdxPtr != IndexDef::END) {
			sql += ",";
		} else {
			sql += ")";
			break;
		}
	}
	return sql;
}

std::string DBAgentSQLite3::makeDropIndexStatement(
  const std::string &name, const std::string &tableName)
{
	return StringUtils::sprintf("DROP INDEX %s", name.c_str());
}

string DBAgentSQLite3::getDBPath(void) const
{
	return m_ctx->dbPath;
}

//
// Non static methods
//
void DBAgentSQLite3::openDatabase(void)
{
	if (m_ctx->db)
		return;

	HATOHOL_ASSERT(!m_ctx->dbPath.empty(), "dbPath is empty.");
	m_ctx->db = openDatabase(m_ctx->dbPath);
}

void DBAgentSQLite3::execSql(const char *fmt, ...)
{
	// make a query string
	MAKE_SQL_STATEMENT_FROM_VAARG(fmt, sql);

	// execute the query
	_execSql(m_ctx->db, sql);
}
