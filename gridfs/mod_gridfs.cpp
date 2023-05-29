/*
	mod_gridfs.c -- Apache 2.2+ module that supports serving of files from MongoDB GridFS.

	See http://www.mongodb.org/ and http://www.mongodb.org/display/DOCS/GridFS for more information.

	See LICENSE file for licensing details.
*/

#include <unistd.h>

#include "client/dbclient.h"
#include "client/gridfs.h"
#include "util/assert_util.h"

#include "apr_strings.h"

#include "httpd.h"
#include "http_log.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_request.h"

//	Declare module
extern "C"
{
	extern module AP_MODULE_DECLARE_DATA gridfs_module;
}

//	Extra Apache 2.4+ module declaration
#ifdef APLOG_USE_MODULE
APLOG_USE_MODULE(gridfs);
#endif
 
//	Filename field types
enum FilenameFieldType
{
	//	String (default)
	FILENAME_FIELD_TYPE_STRING,
	//	ObjectID (default)
	FILENAME_FIELD_TYPE_OBJECTID,
};

//	Default cache max age in seconds
const int DEFAULT_CACHE_MAX_AGE = 604800;

//	Default connect timeout in seconds
const int DEFAULT_CONNECT_TIMEOUT = 30;

//	Maximum cache age in seconds
const int MAX_CACHE_MAX_AGE = 86400 * 365 * 10;

//	Maximum connect timeout in seconds
const int MAX_CONNECT_TIMEOUT = 300;

//	Retry delay in milliseconds
const int RETRY_DELAY = 300;

//	Default prefix
static const std::string DEFAULT_COLLECTION_PREFIX = "fs";

//	Default filename field
static const std::string DEFAULT_FILENAME_FIELD = "filename";

//	Default filename field type
static const FilenameFieldType DEFAULT_FILENAME_FIELD_TYPE = FILENAME_FIELD_TYPE_STRING;

//	Module configuration
struct gridfs_config
{
	const std::string *connection_string;
	const std::string *database;
	const std::string *collection_prefix;
	const std::string *filename_field;
	FilenameFieldType filename_field_type;
	const std::string *username;
	const std::string *password;
	int cache_max_age;
	bool cache_max_age_set;
	int connect_timeout;
	bool connect_timeout_set;
	const std::string *read_pref_mode;
	bool custom_read_pref_mode;
	const mongo::BSONObj* read_pref_tags;
	const std::string *context;
};

//	Creates module configuration
static void *gridfs_create_config(apr_pool_t *const pool, char *const location)
{
	gridfs_config *const config = static_cast<gridfs_config *>(apr_pcalloc(pool, sizeof(gridfs_config)));
	if (config == 0)
		return 0;
	config->filename_field_type = DEFAULT_FILENAME_FIELD_TYPE;
	config->cache_max_age = DEFAULT_CACHE_MAX_AGE;
	config->connect_timeout = DEFAULT_CONNECT_TIMEOUT;
	if (location != 0 && location[0] == '/' && location[1] != '\0')
	{
	  	void *const context_data = apr_palloc(pool, sizeof(std::string));
	  	if (context_data == 0)
	    	return config;
		std::string *context;
		try
		{
			context = new (context_data) std::string(location + 1);
			if (*context->rbegin() != '/')
				context->append(1, '/');
		}
		catch (...)
		{
			return config;
		}
		config->context = context;
	}
	return config;
}

//	Merges module configuration
static void *gridfs_merge_config(apr_pool_t *const pool, void *const basev, void *const addv)
{
	gridfs_config *const config = static_cast<gridfs_config *>(apr_pcalloc(pool, sizeof(gridfs_config)));
	if (config == 0)
		return 0;
	const gridfs_config *const base = static_cast<const gridfs_config *>(basev);
	const gridfs_config *const add = static_cast<const gridfs_config *>(addv);
	config->connection_string = add->connection_string != 0 ? add->connection_string : base->connection_string;
	config->database = add->database != 0 ? add->database : base->database;
	config->collection_prefix = add->collection_prefix != 0 ? add->collection_prefix : base->collection_prefix;
	config->filename_field = add->filename_field != 0 ? add->filename_field : base->filename_field;
	config->filename_field_type = add->filename_field_type != 0 ? add->filename_field_type : base->filename_field_type;
	config->username = add->username != 0 ? add->username : base->username;
	config->password = add->password != 0 ? add->password : base->password;
	config->cache_max_age = add->cache_max_age_set ? add->cache_max_age : base->cache_max_age;
	config->cache_max_age_set = add->cache_max_age_set || base->cache_max_age_set;
	config->connect_timeout = add->connect_timeout_set ? add->connect_timeout : base->connect_timeout;
	config->connect_timeout_set = add->connect_timeout_set || base->connect_timeout_set;
	config->read_pref_mode = add->read_pref_mode != 0 ? add->read_pref_mode : base->read_pref_mode;
	config->custom_read_pref_mode = add->read_pref_mode != 0 ? add->custom_read_pref_mode : base->custom_read_pref_mode;
	config->read_pref_tags = add->read_pref_tags != 0 ? add->read_pref_tags : base->read_pref_tags;
	config->context = add->context != 0 ? add->context : base->context;
	return config;
}

//	Handles "GridFSConnection <connection string>" command
static const char *gridfs_connection_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	void *const connection_string_data = apr_palloc(command->pool, sizeof(std::string));
	if (connection_string_data == 0)
		return "GridFSConnection failed to allocate data.";
	std::string *connection_string;
	try
	{
		connection_string = new (connection_string_data) std::string(argument);
	}
	catch (...)
	{
		return "GridFSConnection exception.";
	}
	config->connection_string = connection_string;
	return 0;
}

//	Handles "GridFSDatabase <database name>" command
static const char *gridfs_database_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	void *const database_data = apr_palloc(command->pool, sizeof(std::string));
	if (database_data == 0)
		return "GridFSDatabase failed to allocate data.";
	std::string *database;
	try
	{
		database = new (database_data) std::string(argument);
	}
	catch (...)
	{
		return "GridFSDatabase exception.";
	}
	config->database = database;
	return 0;
}

//	Handles "GridFSCollectionPrefix <collection prefix>" command
static const char *gridfs_collection_prefix_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	void *const collection_prefix_data = apr_palloc(command->pool, sizeof(std::string));
	if (collection_prefix_data == 0)
		return "GridFSCollectionPrefix failed to allocate data.";
	std::string *collection_prefix;
	try
	{
		collection_prefix = new (collection_prefix_data) std::string(argument);
	}
	catch (...)
	{
		return "GridFSCollectionPrefix exception.";
	}
	config->collection_prefix = collection_prefix;
	return 0;
}

//	Handles "GridFSFilenameField <filename field>" command
static const char *gridfs_filename_field_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	void *const filename_field_data = apr_palloc(command->pool, sizeof(std::string));
	if (filename_field_data == 0)
		return "GridFSFilenameField failed to allocate data.";
	std::string *filename_field;
	try
	{
		filename_field = new (filename_field_data) std::string(argument);
	}
	catch (...)
	{
		return "GridFSFilenameField exception.";
	}
	config->filename_field = filename_field;
	return 0;
}

//	Handles "GridFSFilenameFieldType <filename field type>" command
static const char *gridfs_filename_field_type_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	FilenameFieldType filename_field_type;
	try
	{
		if (strcasecmp("objectid", argument) == 0)
		{
			filename_field_type = FILENAME_FIELD_TYPE_OBJECTID;
		}
		else if (strcasecmp("string", argument) == 0)
		{
			filename_field_type = FILENAME_FIELD_TYPE_STRING;
		}
		else
		{
			return "Invalid GridFSFilenameFieldType.";
		}
	}
	catch (...)
	{
		return "GridFSFilenameFieldType exception.";
	}
	config->filename_field_type = filename_field_type;
	return 0;
}

//	Handles "GridFSUsername <username>" command
static const char *gridfs_username_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	void *const username_data = apr_palloc(command->pool, sizeof(std::string));
	if (username_data == 0)
		return "GridFSUsername failed to allocate data.";
	std::string *username;
	try
	{
		username = new (username_data) std::string(argument);
	}
	catch (...)
	{
		return "GridFSUsername exception.";
	}
	config->username = username;
	return 0;
}

//	Handles "GridFSPassword <password>" command
static const char *gridfs_password_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	void *const password_data = apr_palloc(command->pool, sizeof(std::string));
	if (password_data == 0)
		return "GridFSPassword failed to allocate data.";
	std::string *password;
	try
	{
		password = new (password_data) std::string(argument);
	}
	catch (...)
	{
		return "GridFSPassword exception.";
	}
	config->password = password;
	return 0;
}

//	Handles "GridFSCacheMaxAge <cache max age>" command
static const char *gridfs_cache_max_age_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	const int cache_max_age = std::atoi(argument);
	if (cache_max_age < 0 || cache_max_age > MAX_CACHE_MAX_AGE)
		return "GridFSCacheMaxAge out of range.";
	config->cache_max_age = cache_max_age;
	config->cache_max_age_set = true;
	return 0;
}

//	Handles "GridFSConnectTimeout <connect timeout>" command
static const char *gridfs_connect_timeout_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	int connect_timeout = atoi(argument);
	if (connect_timeout < 0 || connect_timeout > MAX_CONNECT_TIMEOUT)
		return "GridFSConnectTimeout out of range.";
	config->connect_timeout = connect_timeout;
	config->connect_timeout_set = true;
	return 0;
}

//	Handles "GridFSReadPrefMode <read preference mode>" command
static const char *gridfs_read_pref_mode_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	void *const read_pref_mode_data = apr_palloc(command->pool, sizeof(std::string));
	if (read_pref_mode_data == 0)
		return "GridFSReadPrefMode failed to allocate data.";
	std::string *read_pref_mode;
	try
	{
		read_pref_mode = new (read_pref_mode_data) std::string(argument);
	}
	catch (...)
	{
		return "GridFSReadPrefMode exception.";
	}
	config->read_pref_mode = read_pref_mode;
	config->custom_read_pref_mode = *read_pref_mode != "primary";
	return 0;
}

//	Handles "GridFSReadPrefTags <read preference tags>" command
static const char *gridfs_read_pref_tags_command(cmd_parms *const command, void *const module_config, const char *const argument)
{
	gridfs_config *const config = static_cast<gridfs_config *>(module_config);
	void *const read_pref_tags_data = apr_palloc(command->pool, sizeof(mongo::BSONObj));
	if (read_pref_tags_data == 0)
		return "GridFSReadPrefTags failed to allocate data.";
	const mongo::BSONObj *read_pref_tags;
	try
	{
		const std::string json = "{tags:[" + std::string(argument) + "]}";
		const mongo::BSONObj tags = mongo::fromjson(json).getObjectField("tags").copy();
		if (!tags.isEmpty())
			read_pref_tags = new (read_pref_tags_data) mongo::BSONObj(tags);
		else
			read_pref_tags = 0;
	}
	catch (const mongo::MsgAssertionException& exception)
	{
		ap_log_error(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, command->server, 
			"mod_gridfs: Invalid read preference tags '{tags:[%s]}': %s.", 
			argument, exception.toString().c_str());
		return "GridFSReadPrefTags has invalid format, see error log for details.";
	}
	catch (...)
	{
		return "GridFSReadPrefTags exception.";
	}
	config->read_pref_tags = read_pref_tags;
	return 0;
}

//	Handles request
static int gridfs_handler(request_rec *const request)
{
	const gridfs_config *const config = static_cast<gridfs_config *>(ap_get_module_config(request->per_dir_config, &gridfs_module));
	if (config->connection_string == 0 || config->database == 0)
		return DECLINED;
   	request->allowed |= AP_METHOD_BIT << M_GET;
	if (request->method_number != M_GET)
		return DECLINED;
	if (*request->uri != '/' || request->uri[1] == '\0')
		return DECLINED;
	const char * filename = request->uri + 1;
	if (config->context != 0)
	{
	  	const std::string& context = *config->context;
	  	const size_t context_length = context.length();
	  	if (context.compare(0, context_length, filename, context_length) == 0)
		  	filename += context_length;
	}
	filename = apr_pstrdup(request->pool, filename);
	if (filename == 0)
	{
		ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, request, "mod_gridfs: Failed to allocate filename memory."); 
		return HTTP_INTERNAL_SERVER_ERROR;
	}
	const std::string& database = *config->database;
	apr_bucket_brigade *brigade = 0;
	try
	{
		apr_off_t content_length = 0;
		std::string content_type;
		bool custom_read_pref_mode = config->custom_read_pref_mode;
		if (!custom_read_pref_mode && config->read_pref_tags != 0)
			ap_log_rerror(APLOG_MARK, APLOG_WARNING | APLOG_NOERRNO, 0, request, "mod_gridfs: Read preference tags configured for incompatible mode (file: '%s').", filename); 
		const apr_time_t retry_threshold = request->request_time + apr_time_from_sec(config->connect_timeout);
		while (true)
		{
			try
			{
				std::auto_ptr<mongo::ScopedDbConnection> connection(mongo::ScopedDbConnection::getScopedDbConnection(*config->connection_string, config->connect_timeout));

				mongo::DBClientBase& client = connection->conn();
				if (config->username != 0 && config->password != 0)
				{
					const std::string& username = *config->username;
					std::string errmsg;
					if (!client.auth(database, username, *config->password, errmsg))
					{
						connection->done();
						ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, request, "mod_gridfs: Failed to authenticate user '%s' (database: '%s', file: '%s'): %s.", 
							username.c_str(), database.c_str(), filename, errmsg.c_str());
						return HTTP_INTERNAL_SERVER_ERROR;
					}
				}

				const std::string& collection_prefix = config->collection_prefix != 0 ? *config->collection_prefix : DEFAULT_COLLECTION_PREFIX;
				const std::string& filename_field = config->filename_field != 0 ? *config->filename_field : DEFAULT_FILENAME_FIELD;
				const mongo::GridFS gridfs(client, database, collection_prefix, false);
				mongo::BSONObj query;
				try
				{
					switch (config->filename_field_type)
					{
						case FILENAME_FIELD_TYPE_STRING:
							query = BSON(filename_field << filename);
							break;
    
						case FILENAME_FIELD_TYPE_OBJECTID:
							query = BSON(filename_field << mongo::OID(filename));
							break;
    
						default:
							ap_log_rerror(APLOG_MARK, APLOG_CRIT | APLOG_NOERRNO, 0, request, "mod_gridfs: Invalid GridFSFilenameFieldType configuration (file: '%s').", filename); 
							return HTTP_INTERNAL_SERVER_ERROR;
					}
				}
				catch (const mongo::DBException& exception)
				{
					return DECLINED;
				}
				while (true)
				{
					try
					{
						mongo::GridFile gridfile = custom_read_pref_mode ?
							gridfs.findFile(query, *config->read_pref_mode, config->read_pref_tags) :
							gridfs.findFile(query);
						if (!gridfile.exists())
						{
							if (custom_read_pref_mode)
							{
								custom_read_pref_mode = false;
								gridfile = gridfs.findFile(filename);
								if (!gridfile.exists())
								{
									connection->done();
									return DECLINED;
								}
								ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, request, "mod_gridfs: Reseted read preference due to missing file '%s' (database: '%s').", filename, database.c_str());
							}
							else
							{
								connection->done();
								return DECLINED;
							}
						}
						const mongo::Date_t upload_date = gridfile.getUploadDate();
						request->mtime = apr_time_from_sec(upload_date.toTimeT());
						ap_set_last_modified(request);
						const std::string& md5 = gridfile.getMD5();
						if (!md5.empty())
							apr_table_setn(request->headers_out, "ETag", md5.c_str());
						if (ap_meets_conditions(request) == HTTP_NOT_MODIFIED)
						{
							connection->done();
							if (!md5.empty())
								apr_table_unset(request->headers_out, "ETag");
							return HTTP_NOT_MODIFIED;
						}
						const mongo::gridfs_offset file_length = gridfile.getContentLength();
						content_length = 0;
						content_type = gridfile.getContentType();
						if (file_length != 0 && request->header_only == 0)
						{
							const int num_chunks = gridfile.getNumChunks();
							if (num_chunks == 0)
							{
								connection->done();
								ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, request, "mod_gridfs: No chunks available for file '%s' (database: '%s').", filename, database.c_str()); 
								return HTTP_INTERNAL_SERVER_ERROR;
							}
							if (brigade == 0)
							{
								brigade = apr_brigade_create(request->pool, request->connection->bucket_alloc);
								if (brigade == 0)
								{
									connection->done();
									ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, request, "mod_gridfs: Failed to create brigade."); 
									return HTTP_INTERNAL_SERVER_ERROR;
								}
							}
							for (int chunk_index = 0;chunk_index < num_chunks;chunk_index++) 
							{
								const mongo::GridFSChunk& chunk = gridfile.getChunk(chunk_index);
								int chunk_length;
								const char *chunk_data = chunk.data(chunk_length);
								if (chunk_length == 0)
									continue;
								const int result = apr_brigade_write(brigade, 0, 0, chunk_data, chunk_length);
								if (result != APR_SUCCESS)
								{
									connection->done();
									ap_log_rerror(APLOG_MARK, APLOG_ERR, result, request, "mod_gridfs: Failed to write chunk %d for file '%s' to brigade (length: %d).", chunk_index, filename, chunk_length); 
									return HTTP_INTERNAL_SERVER_ERROR;
								}
								content_length += chunk_length;
							}
							if (content_length != file_length)
							{
								connection->done();
								ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, request, "mod_gridfs: Mismatching files/chunks length for file '%s' (difference: %d, database: '%s').", filename, static_cast<int>(file_length - content_length), database.c_str());
								return HTTP_INTERNAL_SERVER_ERROR;
							}
						}
						break;
					}
					catch (const mongo::DBException& exception)
					{
						const int code = exception.getCode();
						switch (code)
						{
						case 10014:	//	chunk is empty!
							if (custom_read_pref_mode)
							{
								custom_read_pref_mode = false;
								ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, request, "mod_gridfs: Reseted read preference due to empty chunk for file '%s' (database: '%s').", filename, database.c_str());
								break;
							}
							ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, request, "mod_gridfs: Missing chunk for file '%s' (database: '%s').", filename, database.c_str());
							connection->done();
							return HTTP_NOT_FOUND;
	        	
						default:
							throw;
						}
					}
				}
				connection->done();
				break;
			}
			catch (const mongo::DBException& exception)
			{
				if (apr_time_now() >= retry_threshold)
					throw;
				const int code = exception.getCode();
				switch (code)
				{
				case 13106:	//	no matching nodes
					if (!custom_read_pref_mode)
						throw;
					custom_read_pref_mode = false;
					ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, request, "mod_gridfs: Retrying after tag set MongoDB exception for file '%s' (code: %d, database: '%s'): %s.", filename, code, database.c_str(), exception.what()); 
					break;

				case 7:		//	mongos missing primary
				case 9001:	//	default socket exception
				case 10009:	//	ReplicaSetMonitor no master found for set
				case 10276:	//	DBClientBase::findN: transport error
				case 11002:	//	pool socket exception
				case 14827:	//	mongos can't contact shard
					ap_log_rerror(APLOG_MARK, APLOG_WARNING | APLOG_NOERRNO, 0, request, "mod_gridfs: Retrying after MongoDB exception for file '%s' (code: %d, database: '%s'): %s.", filename, code, database.c_str(), exception.what()); 
					break;

				default:
					throw;
				}
			}
			if (brigade != 0)
				apr_brigade_cleanup(brigade);
			apr_sleep(RETRY_DELAY * 1000);
		}
		request->filename = const_cast<char *>(filename);
		if (content_length != 0)
			ap_set_content_length(request, content_length);
		if (content_type.empty())
		{
			request->finfo.filetype = APR_REG;
			const int result = ap_run_type_checker(request);
			if (result != APR_SUCCESS)
				ap_log_rerror(APLOG_MARK, APLOG_WARNING, result, request, "mod_gridfs: Failed to run type checker for file '%s' (database: '%s').", filename, database.c_str()); 
		}
		else
			ap_set_content_type(request, content_type.c_str());
		if (config->cache_max_age != 0)
		{
			char cache_control[32];
			snprintf(cache_control, sizeof(cache_control) - 1, "public, max-age=%d", config->cache_max_age);
			apr_table_setn(request->headers_out, "Cache-Control", cache_control);
			apr_time_t expires_time = request->request_time + apr_time_from_sec(config->cache_max_age);
			char expires[APR_RFC822_DATE_LEN];
			apr_rfc822_date(expires, expires_time);
			apr_table_setn(request->headers_out, "Expires", expires);
		}
	}
	catch (const mongo::DBException& exception)
	{
		ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, request, "mod_gridfs: Unhandled MongoDB exception occured for file '%s' (code: %d, database: '%s'): %s.", filename, exception.getCode(), database.c_str(), exception.what()); 
		return HTTP_INTERNAL_SERVER_ERROR;
	}
	catch (const std::exception& exception)
	{
		ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, request, "mod_gridfs: Unhandled exception occured for file '%s' (database: '%s'): %s.", filename, database.c_str(), exception.what()); 
		return HTTP_INTERNAL_SERVER_ERROR;
	}
	catch (...)
	{
		ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, request, "mod_gridfs: Unknown unhandled exception occured for file '%s' (database: '%s').", filename, database.c_str()); 
		return HTTP_INTERNAL_SERVER_ERROR;
	}
	if (brigade != 0)
	    return ap_pass_brigade(request->output_filters, brigade);
	return OK;
}

//	Registers hooks
static void gridfs_register_hooks(apr_pool_t *const pool)
{
	ap_hook_handler(gridfs_handler, 0, 0, APR_HOOK_MIDDLE);
}

//	Describes module configuration commands
static const command_rec gridfs_commands[] =
{
	AP_INIT_TAKE1("GridFSConnection", reinterpret_cast<cmd_func>(gridfs_connection_command), 0, OR_FILEINFO, "GridFS connection string."),
	AP_INIT_TAKE1("GridFSDatabase", reinterpret_cast<cmd_func>(gridfs_database_command), 0, OR_FILEINFO, "GridFS database name."),
	AP_INIT_TAKE1("GridFSCollectionPrefix", reinterpret_cast<cmd_func>(gridfs_collection_prefix_command), 0, OR_FILEINFO, "GridFS collection prefix."),
	AP_INIT_TAKE1("GridFSFilenameField", reinterpret_cast<cmd_func>(gridfs_filename_field_command), 0, OR_FILEINFO, "GridFS file name field."),
	AP_INIT_TAKE1("GridFSFilenameFieldType", reinterpret_cast<cmd_func>(gridfs_filename_field_type_command), 0, OR_FILEINFO, "GridFS file name field type."),
	AP_INIT_TAKE1("GridFSUsername", reinterpret_cast<cmd_func>(gridfs_username_command), 0, OR_FILEINFO, "GridFS database authentication username."),
	AP_INIT_TAKE1("GridFSPassword", reinterpret_cast<cmd_func>(gridfs_password_command), 0, OR_FILEINFO, "GridFS database authentication password."),	
	AP_INIT_TAKE1("GridFSCacheMaxAge", reinterpret_cast<cmd_func>(gridfs_cache_max_age_command), 0, OR_FILEINFO, "GridFS cache max age (seconds, 0 to disable expiration)."),
	AP_INIT_TAKE1("GridFSConnectTimeout", reinterpret_cast<cmd_func>(gridfs_connect_timeout_command), 0, OR_FILEINFO, "GridFS connect timeout (seconds, 0 for infinite)."),
	AP_INIT_TAKE1("GridFSReadPrefMode", reinterpret_cast<cmd_func>(gridfs_read_pref_mode_command), 0, OR_FILEINFO, "GridFS read preference mode."),	
	AP_INIT_TAKE1("GridFSReadPrefTags", reinterpret_cast<cmd_func>(gridfs_read_pref_tags_command), 0, OR_FILEINFO, "GridFS read preference tags."),	
	{0}
};

//	Defines module
extern "C"
{
	module AP_MODULE_DECLARE_DATA gridfs_module =
	{
		STANDARD20_MODULE_STUFF,
		gridfs_create_config,
		gridfs_merge_config,
		0,
		0,
		gridfs_commands,
		gridfs_register_hooks
	};
}
