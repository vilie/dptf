/******************************************************************************
** Copyright (c) 2013-2015 Intel Corporation All Rights Reserved
**
** Licensed under the Apache License, Version 2.0 (the "License"); you may not
** use this file except in compliance with the License.
**
** You may obtain a copy of the License at
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**
** See the License for the specific language governing permissions and
** limitations under the License.
**
******************************************************************************/

#define ESIF_TRACE_ID	ESIF_TRACEMODULE_UF

/* ESIF */
#include "esif_uf.h"		/* ESIF Upper Framework */

/* Managers */
#include "esif_pm.h"		/* Participant Manager */
#include "esif_uf_actmgr.h"	/* Action Manager */
#include "esif_uf_appmgr.h"	/* Application Manager */
#include "esif_uf_cfgmgr.h"	/* Config Manager */
#include "esif_uf_cnjmgr.h"	/* Conjure Manager */
#include "esif_uf_version.h"
#include "esif_uf_eventmgr.h"

/* Init */
#include "esif_dsp.h"		/* Device Support Package */
#include "esif_link_list.h"
#include "esif_hash_table.h"
#include "esif_uf_logging.h"

/* IPC */
#include "esif_command.h"	/* Command Interface */
#include "esif_event.h"		/* Events */
#include "esif_ipc.h"		/* IPC */
#include "esif_uf_ipc.h"	/* IPC */
#include "esif_ws_server.h"	/* Web Server */

#include "esif_lib_databank.h"
#include "esif_ccb_timer.h"




/* Native memory allocation functions for use by memtrace functions only */
#define native_malloc(siz)          malloc(siz)
#define native_realloc(ptr, siz)    realloc(ptr, siz)
#define native_free(ptr)            free(ptr)

#ifdef ESIF_ATTR_OS_WINDOWS
#include <share.h>
//
// The Windows banned-API check header must be included after all other headers, or issues can be identified
// against Windows SDK/DDK included headers which we have no control over.
//
#define _SDL_BANNED_RECOMMENDED
#include "win\banned.h"
#endif

int g_errorlevel = 0;			// Exit Errorlevel
int g_quit		 = ESIF_FALSE;	// Quit
int g_disconnectClient = ESIF_FALSE;// Disconnect client

UInt8 g_esif_started = ESIF_FALSE;

/* ESIF Memory Pool */
struct esif_ccb_mempool *g_mempool[ESIF_MEMPOOL_TYPE_MAX] = {0};
esif_ccb_lock_t g_mempool_lock;

static esif_thread_t g_webthread;  //web worker thread

// Global Shell lock to limit parse_cmd to one thread at a time
#ifdef ESIF_ATTR_SHELL_LOCK
esif_ccb_mutex_t g_shellLock;
#endif

#ifdef ESIF_ATTR_MEMTRACE
struct memtrace_s g_memtrace;
#endif

// ESIF Log File object
typedef struct EsifLogFile_s {
	esif_ccb_lock_t lock;		// Thread Lock
	esif_string		name;		// Log Name
	esif_string		filename;	// Log file name
	FILE			*handle;	// Log file handle or NULL if not open
} EsifLogFile;

static EsifLogFile g_EsifLogFile[MAX_ESIFLOG] = {0};

enum esif_rc esif_pathlist_init(esif_string path_list);
void esif_pathlist_exit(void);
int esif_pathlist_count(void);

// Optional Custom path configuration file support
#ifdef ESIF_ATTR_OS_WINDOWS
# define ESIF_PATHLIST_FILENAME	"C:\\Windows\\esif.conf"	
#else
# define ESIF_PATHLIST_FILENAME	"/etc/esif.conf"
#endif

eEsifError EsifLogMgrInit(void)
{
	int j;

	ESIF_TRACE_ENTRY_INFO();

	esif_ccb_memset(g_EsifLogFile, 0, sizeof(g_EsifLogFile));
	for (j=0; j < MAX_ESIFLOG; j++) {
		esif_ccb_lock_init(&g_EsifLogFile[j].lock);
	}
	g_EsifLogFile[ESIF_LOG_EVENTLOG].name = esif_ccb_strdup("event");
	g_EsifLogFile[ESIF_LOG_DEBUGGER].name = esif_ccb_strdup("debug");
	g_EsifLogFile[ESIF_LOG_SHELL].name    = esif_ccb_strdup("shell");
	g_EsifLogFile[ESIF_LOG_TRACE].name    = esif_ccb_strdup("trace");
	g_EsifLogFile[ESIF_LOG_UI].name       = esif_ccb_strdup("ui");

	ESIF_TRACE_EXIT_INFO();
	return ESIF_OK;
}

void EsifLogMgrExit(void)
{
	int j;

	ESIF_TRACE_ENTRY_INFO();

	for (j=0; j < MAX_ESIFLOG; j++) {
		if (g_EsifLogFile[j].handle != NULL) {
			esif_ccb_fclose(g_EsifLogFile[j].handle);
		}
		esif_ccb_free(g_EsifLogFile[j].name);
		esif_ccb_free(g_EsifLogFile[j].filename);
		esif_ccb_lock_uninit(&g_EsifLogFile[j].lock);
	}
	esif_ccb_memset(g_EsifLogFile, 0, sizeof(g_EsifLogFile));

	ESIF_TRACE_EXIT_INFO();
}

int EsifLogFile_Open(EsifLogType type, const char *filename, int append)
{
	int rc=0;
	char fullpath[MAX_PATH]={0};
	char mode[3] = {(append ? 'a' : 'w'), 0, 0};

	esif_ccb_write_lock(&g_EsifLogFile[type].lock);
	if (g_EsifLogFile[type].handle != NULL)
		esif_ccb_fclose(g_EsifLogFile[type].handle);

	EsifLogFile_GetFullPath(fullpath, sizeof(fullpath), filename);
#ifdef ESIF_ATTR_OS_WINDOWS
	mode[1] = 'c';
	g_EsifLogFile[type].handle = _fsopen(fullpath, mode, _SH_DENYWR);
	if (g_EsifLogFile[type].handle == NULL)
		rc = errno;
#else
	rc = esif_ccb_fopen(&g_EsifLogFile[type].handle, fullpath, mode);
#endif
	if (rc == 0) {
		esif_ccb_free(g_EsifLogFile[type].filename);
		g_EsifLogFile[type].filename = esif_ccb_strdup((char *)fullpath);
	}
	esif_ccb_write_unlock(&g_EsifLogFile[type].lock);
	return rc;
}

int EsifLogFile_Close(EsifLogType type)
{
	int rc = EOF;

	esif_ccb_write_lock(&g_EsifLogFile[type].lock);
	if (g_EsifLogFile[type].handle != NULL)
		rc = esif_ccb_fclose(g_EsifLogFile[type].handle);
	g_EsifLogFile[type].handle = NULL;
	esif_ccb_write_unlock(&g_EsifLogFile[type].lock);
	return rc;
}

int EsifLogFile_IsOpen(EsifLogType type)
{
	int rc=0;
	esif_ccb_read_lock(&g_EsifLogFile[type].lock);
	rc = (g_EsifLogFile[type].handle != NULL);
	esif_ccb_read_unlock(&g_EsifLogFile[type].lock);
	return rc;
}

int EsifLogFile_Write(EsifLogType type, const char *fmt, ...)
{
	int rc = 0;
	va_list args;

	va_start(args, fmt);
	esif_ccb_write_lock(&g_EsifLogFile[type].lock);
	if (g_EsifLogFile[type].handle != NULL) {
		rc = esif_ccb_vfprintf(g_EsifLogFile[type].handle, fmt, args);
		fflush(g_EsifLogFile[type].handle);
	}
	esif_ccb_write_unlock(&g_EsifLogFile[type].lock);
	va_end(args);
	return rc;
}

int EsifLogFile_WriteArgs(EsifLogType type, const char *fmt, va_list args)
{
	int rc = 0;
	
	esif_ccb_write_lock(&g_EsifLogFile[type].lock);
	if (g_EsifLogFile[type].handle != NULL) {
		rc = esif_ccb_vfprintf(g_EsifLogFile[type].handle, fmt, args);
		fflush(g_EsifLogFile[type].handle);
	}
	esif_ccb_write_unlock(&g_EsifLogFile[type].lock);
	return rc;
}

esif_string EsifLogFile_GetFullPath(esif_string buffer, size_t buf_len, const char *filename)
{
	char *sep = strrchr((char *)filename, *ESIF_PATH_SEP);
	if (sep != NULL)
		filename = sep+1; // Ignore folders and use file.ext only

	esif_build_path(buffer, buf_len, ESIF_PATHTYPE_LOG, (esif_string)filename, NULL);
	return buffer;
}

void EsifLogFile_DisplayList(void)
{
	int j;
	for (j = 0; j < MAX_ESIFLOG; j++) {
		if (g_EsifLogFile[j].handle != NULL && g_EsifLogFile[j].name != NULL) {
			CMD_OUT("%s log: %s\n", g_EsifLogFile[j].name, (g_EsifLogFile[j].filename ? g_EsifLogFile[j].filename : "NA"));
		}
	}
}

EsifLogType EsifLogType_FromString(const char *name)
{
	EsifLogType result = (EsifLogType)0;
	if (NULL == name)
		return result;
	else if (esif_ccb_strnicmp(name, "eventlog", 5)==0)
		result = ESIF_LOG_EVENTLOG;
	else if (esif_ccb_strnicmp(name, "debugger", 5)==0)
		result = ESIF_LOG_DEBUGGER;
	else if (esif_ccb_stricmp(name, "shell")==0)
		result = ESIF_LOG_SHELL;
	else if (esif_ccb_stricmp(name, "trace")==0)
		result = ESIF_LOG_TRACE;
	else if (esif_ccb_stricmp(name, "ui")==0)
		result = ESIF_LOG_UI;
	return result;
}

#ifdef ESIF_ATTR_OS_WINDOWS
extern enum esif_rc ESIF_CALLCONV write_to_srvr_cnsl_intfc_varg(const char *pFormat, va_list args);
# define EsifConsole_vprintf(fmt, args)		write_to_srvr_cnsl_intfc_varg(fmt, args)
#else
# define EsifConsole_vprintf(fmt, args)		vprintf(fmt, args)
#endif

int EsifConsole_WriteTo(u32 writeto, const char *format, ...)
{
	int rc=0;

	if (writeto & CMD_WRITETO_CONSOLE) {
		va_list args;
		va_start(args, format);
		rc = EsifConsole_vprintf(format, args);
		va_end(args);
	}
	if ((writeto & CMD_WRITETO_LOGFILE) && (g_EsifLogFile[ESIF_LOG_SHELL].handle != NULL)) {
		va_list args;
		va_start(args, format);
		esif_ccb_write_lock(&g_EsifLogFile[ESIF_LOG_SHELL].lock);
		rc = esif_ccb_vfprintf(g_EsifLogFile[ESIF_LOG_SHELL].handle, format, args);
		esif_ccb_write_unlock(&g_EsifLogFile[ESIF_LOG_SHELL].lock);
		va_end(args);
	}
	return rc;
}

int EsifConsole_WriteConsole(const char *format, va_list args)
{
	return EsifConsole_vprintf(format, args);
}

int EsifConsole_WriteLogFile(const char *format, va_list args)
{
	int rc=0;
	esif_ccb_write_lock(&g_EsifLogFile[ESIF_LOG_SHELL].lock);
	rc = esif_ccb_vfprintf(g_EsifLogFile[ESIF_LOG_SHELL].handle, format, args);
	esif_ccb_write_unlock(&g_EsifLogFile[ESIF_LOG_SHELL].lock);
	return rc;
}

/* Work Around */
static enum esif_rc get_participant_data(
	struct esif_ipc_event_data_create_participant *pi_ptr,
	UInt8 participantId
	)
{
	eEsifError rc = ESIF_OK;

	struct esif_command_get_part_detail *data_ptr = NULL;
	struct esif_ipc_command *command_ptr = NULL;
	struct esif_ipc *ipc_ptr = NULL;
	const u32 data_len = sizeof(struct esif_command_get_part_detail);

	ipc_ptr = esif_ipc_alloc_command(&command_ptr, data_len);
	if (NULL == ipc_ptr || NULL == command_ptr) {
		ESIF_TRACE_ERROR("Fail to allocate esif_ipc/esif_ipc_command\n");
		rc = ESIF_E_NO_MEMORY;
		goto exit;
	}

	command_ptr->type = ESIF_COMMAND_TYPE_GET_PARTICIPANT_DETAIL;
	command_ptr->req_data_type   = ESIF_DATA_UINT32;
	command_ptr->req_data_offset = 0;
	command_ptr->req_data_len    = 4;
	command_ptr->rsp_data_type   = ESIF_DATA_STRUCTURE;
	command_ptr->rsp_data_offset = 0;
	command_ptr->rsp_data_len    = data_len;

	// ID For Command
	*(u32 *)(command_ptr + 1) = participantId;
	rc = ipc_execute(ipc_ptr);

	if (ESIF_OK != rc) {
		goto exit;
	}

	if (ESIF_OK != ipc_ptr->return_code) {
		rc = ipc_ptr->return_code;
		ESIF_TRACE_WARN("ipc_ptr return_code failure - %s\n", esif_rc_str(rc));
		goto exit;
	}

	if (ESIF_OK != command_ptr->return_code) {
		rc = command_ptr->return_code;
		ESIF_TRACE_WARN("command_ptr return_code failure - %s\n", esif_rc_str(rc));
		goto exit;
	}

	// our data
	data_ptr = (struct esif_command_get_part_detail *)(command_ptr + 1);
	if (0 == data_ptr->version) {
		ESIF_TRACE_ERROR("Participant version is 0\n");
		goto exit;
	}

	pi_ptr->id = (u8)data_ptr->id;
	pi_ptr->version = data_ptr->version;
	esif_ccb_memcpy(pi_ptr->class_guid, data_ptr->class_guid, ESIF_GUID_LEN);

	pi_ptr->enumerator = data_ptr->enumerator;
	pi_ptr->flags = data_ptr->flags;

	esif_ccb_strcpy(pi_ptr->name, data_ptr->name, ESIF_NAME_LEN);
	esif_ccb_strcpy(pi_ptr->desc, data_ptr->desc, ESIF_DESC_LEN);
	esif_ccb_strcpy(pi_ptr->driver_name, data_ptr->driver_name, ESIF_NAME_LEN);
	esif_ccb_strcpy(pi_ptr->device_name, data_ptr->device_name, ESIF_NAME_LEN);
	esif_ccb_strcpy(pi_ptr->device_path, data_ptr->device_path, ESIF_NAME_LEN);

	/* ACPI */
	esif_ccb_strcpy(pi_ptr->acpi_device, data_ptr->acpi_device, ESIF_NAME_LEN);
	esif_ccb_strcpy(pi_ptr->acpi_scope, data_ptr->acpi_scope, ESIF_SCOPE_LEN);
	esif_ccb_strcpy(pi_ptr->acpi_uid, data_ptr->acpi_uid, sizeof(pi_ptr->acpi_uid));
	pi_ptr->acpi_type = data_ptr->acpi_type;

	/* PCI */
	pi_ptr->pci_vendor     = data_ptr->pci_vendor;
	pi_ptr->pci_device     = data_ptr->pci_device;
	pi_ptr->pci_bus        = data_ptr->pci_bus;
	pi_ptr->pci_bus_device = data_ptr->pci_bus_device;
	pi_ptr->pci_function   = data_ptr->pci_function;
	pi_ptr->pci_revision   = data_ptr->pci_revision;
	pi_ptr->pci_class      = data_ptr->pci_class;
	pi_ptr->pci_sub_class  = data_ptr->pci_sub_class;
	pi_ptr->pci_prog_if    = data_ptr->pci_prog_if;

exit:

	if (NULL != ipc_ptr) {
		esif_ipc_free(ipc_ptr);
	}
	return rc;
}


/* Will sync any existing lower framework participatnts */
enum esif_rc sync_lf_participants()
{
	eEsifError rc = ESIF_OK;
	struct esif_command_get_participants *data_ptr = NULL;
	const UInt32 data_len = sizeof(struct esif_command_get_participants);
	struct esif_ipc_command *command_ptr = NULL;
	UInt8 i = 0;
	UInt32 count = 0;
	struct esif_ipc *ipc_ptr = NULL;
	
	ESIF_TRACE_ENTRY_INFO();
	
	ipc_ptr = esif_ipc_alloc_command(&command_ptr, data_len);
	if (NULL == ipc_ptr || NULL == command_ptr) {
		ESIF_TRACE_ERROR("Fail to allocate esif_ipc/esif_ipc_command\n");
		rc = ESIF_E_NO_MEMORY;
		goto exit;
	}

	command_ptr->type = ESIF_COMMAND_TYPE_GET_PARTICIPANTS;
	command_ptr->req_data_type   = ESIF_DATA_VOID;
	command_ptr->req_data_offset = 0;
	command_ptr->req_data_len    = 0;
	command_ptr->rsp_data_type   = ESIF_DATA_STRUCTURE;
	command_ptr->rsp_data_offset = 0;
	command_ptr->rsp_data_len    = data_len;

	rc = ipc_execute(ipc_ptr);
	if (ESIF_OK != rc) {
		goto exit;
	}

	if (ESIF_OK != ipc_ptr->return_code) {
		rc = ipc_ptr->return_code;
		ESIF_TRACE_WARN("ipc_ptr return_code failure - %s\n", esif_rc_str(rc));
		goto exit;
	}

	if (ESIF_OK != command_ptr->return_code) {
		rc = command_ptr->return_code;
		ESIF_TRACE_WARN("command_ptr return_code failure - %s\n", esif_rc_str(rc));
		goto exit;
	}

	/* Participant Data */
	data_ptr = (struct esif_command_get_participants *)(command_ptr + 1);
	count    = data_ptr->count;

	for (i = 0; i < count; i++) {
		struct esif_ipc_event_data_create_participant participantData;
		EsifData esifParticipantData = {ESIF_DATA_BINARY, &participantData, sizeof(participantData), sizeof(participantData)};

		rc = get_participant_data(&participantData, i);
		if (ESIF_OK != rc) {
			rc = ESIF_OK; /* Ignore RC for get_participant_data */
			continue;
		}
		EsifEventMgr_SignalEvent(0, EVENT_MGR_DOMAIN_D0, ESIF_EVENT_PARTICIPANT_CREATE, &esifParticipantData);
	}
exit:
	ESIF_TRACE_INFO("rc = %s(%u)", esif_rc_str(rc), rc);

	if (NULL != ipc_ptr) {
		esif_ipc_free(ipc_ptr);
	}

	ESIF_TRACE_EXIT_INFO_W_STATUS(rc);
	return rc;
}


void done(const void *context)
{
	CMD_OUT("Context = %p %s\n", context, (char *)context);
}

// Name to Pathtype Map
typedef struct esif_pathmap_s {
	esif_string		name;
	esif_pathtype	type;
} esif_pathmap;

// Pathname List
typedef struct esif_pathlist_s {
		int				initialized;
		esif_pathmap	*pathmap;
		int				num_paths;
		esif_string		*pathlist;
} esif_pathlist;

static esif_pathlist g_pathlist;

// Initialize Pathname List
enum esif_rc esif_pathlist_init(esif_string paths)
{
	static esif_pathmap pathmap[] = {
		{ "HOME",	ESIF_PATHTYPE_HOME },
		{ "TEMP",	ESIF_PATHTYPE_TEMP },
		{ "DV",		ESIF_PATHTYPE_DV },
		{ "LOG",	ESIF_PATHTYPE_LOG },
		{ "BIN",	ESIF_PATHTYPE_BIN },
		{ "LOCK",	ESIF_PATHTYPE_LOCK },
		{ "EXE",	ESIF_PATHTYPE_EXE },
		{ "DLL",	ESIF_PATHTYPE_DLL },
		{ "DPTF",	ESIF_PATHTYPE_DPTF },
		{ "DSP",	ESIF_PATHTYPE_DSP },
		{ "CMD",	ESIF_PATHTYPE_CMD },
		{ "UI",		ESIF_PATHTYPE_UI  },
		{ NULL }
	};
	static int num_paths = (sizeof(pathmap)/sizeof(*pathmap)) - 1;
	esif_string *pathlist = NULL;
	enum esif_rc rc = ESIF_OK;

	if (!g_pathlist.initialized) {
		char *buffer = NULL;
		char *filename = ESIF_PATHLIST_FILENAME;
		FILE *fp = NULL;
		struct stat st={0};

		// Use pathlist file, if it exists
		if (esif_ccb_stat(filename, &st) == 0 && esif_ccb_fopen(&fp, filename, "rb") == 0) {
			if ((buffer = (char *) esif_ccb_malloc(st.st_size + 1)) != NULL) {
				if (esif_ccb_fread(buffer, st.st_size, sizeof(char), st.st_size, fp) != (size_t)st.st_size) {
					rc = ESIF_E_IO_ERROR;
				}
			}
			esif_ccb_fclose(fp);
		}
		// Otherwise, use default pathlist
		else if (buffer == NULL && paths != NULL) {
			buffer = esif_ccb_strdup(paths);
		}

		if ((buffer == NULL) || ((pathlist = (esif_string *)esif_ccb_malloc(num_paths * sizeof(esif_string))) == NULL)) {
			esif_ccb_free(buffer);
			buffer = NULL;
			rc = ESIF_E_NO_MEMORY;
		}

		// Parse key/value pairs into pathlist
		if (rc == ESIF_OK && buffer != NULL) {
			char *ctxt = 0;
				char *keypair = esif_ccb_strtok(buffer, "\r\n", &ctxt);
				char *value = 0;
				int  id=0;

				g_pathlist.initialized = 1;
				g_pathlist.pathmap = pathmap;
				g_pathlist.num_paths = num_paths;
				g_pathlist.pathlist = pathlist;

				while (keypair != NULL) {
					value = esif_ccb_strchr(keypair, '=');
					if (value) {
						*value++ = 0;
						for (id = 0; id < g_pathlist.num_paths && g_pathlist.pathmap[id].name; id++) {
							if (esif_ccb_stricmp(keypair, g_pathlist.pathmap[id].name) == 0) {
								esif_pathlist_set(g_pathlist.pathmap[id].type, value);
								break;
							}
						}
					}
					keypair = esif_ccb_strtok(NULL, "\r\n", &ctxt);
				}
		}
		esif_ccb_free(buffer);
	}
	return rc;
}

// Uninitialize Pathname List
void esif_pathlist_exit(void)
{
	int j;
	for (j = 0; g_pathlist.pathlist != NULL && j < g_pathlist.num_paths; j++) {
		esif_ccb_free(g_pathlist.pathlist[j]);
	}
	esif_ccb_free(g_pathlist.pathlist);
	memset(&g_pathlist, 0, sizeof(g_pathlist));
}

// Set a Pathname
enum esif_rc  esif_pathlist_set(esif_pathtype type, esif_string value)
{
	enum esif_rc rc = ESIF_E_PARAMETER_IS_NULL;
	if (value != NULL && g_pathlist.pathlist != NULL && type < g_pathlist.num_paths) {
		esif_string pathvalue = esif_ccb_strdup(value);
		if (pathvalue == NULL) {
			rc = ESIF_E_NO_MEMORY;
		}
		else {
			esif_ccb_free(g_pathlist.pathlist[type]);
			g_pathlist.pathlist[type] = pathvalue;
			rc = ESIF_OK;
		}
	}
	return rc;
}

// Get a Pathname
esif_string esif_pathlist_get(esif_pathtype type)
{
	esif_string result = NULL;
	if (g_pathlist.pathlist && type < g_pathlist.num_paths) {
		result = g_pathlist.pathlist[type];
	}
	return result;
}

// Return Pathname List count
int esif_pathlist_count(void)
{
	return g_pathlist.num_paths;
}

// Build Full Pathname for a given path type including an optional filename and extention. 
esif_string esif_build_path(
	esif_string buffer, 
	size_t buf_len,
	esif_pathtype type,
	esif_string filename,
	esif_string ext)
{
	char *pathname = NULL;

	if (NULL == buffer) {
		return NULL;
	}

	// Get Pathname. Do not return full path in result string if it starts with a "#" unless no filename specified
	if ((pathname = esif_pathlist_get(type)) != NULL) {
		if (*pathname == '#') {
			if (filename == NULL && ext == NULL)
				pathname++;
			else
				pathname = "";
		}
		esif_ccb_strcpy(buffer, pathname, buf_len);
	}

	// Create folder if necessary
	if (buffer[0] != '\0') {
		esif_ccb_makepath(buffer);
	}

	// Append Optional filename.ext
	if (filename != NULL || ext != NULL) {
		if (buffer[0] != '\0')
			esif_ccb_strcat(buffer, ESIF_PATH_SEP, buf_len);
		if (filename != NULL)
			esif_ccb_strcat(buffer, filename, buf_len);
		if (ext != NULL)
			esif_ccb_strcat(buffer, ext, buf_len);
	}
	if (buffer[0] == '\0') {
		buffer = NULL;
	}
	return buffer;
}

// WebSocket Server Implemented?
#ifdef ESIF_ATTR_WEBSOCKET

static void *ESIF_CALLCONV esif_web_worker_thread(void *ptr)
{
	UNREFERENCED_PARAMETER(ptr);

	ESIF_TRACE_ENTRY_INFO();
	esif_ws_init();
	ESIF_TRACE_EXIT_INFO();
	return 0;
}

eEsifError EsifWebStart()
{
	
	if (!EsifWebIsStarted()) {
		esif_ccb_thread_create(&g_webthread, esif_web_worker_thread, NULL);
	}
	return ESIF_OK;
}

void EsifWebStop()
{
	if (EsifWebIsStarted()) {
		esif_ws_exit(&g_webthread);
	}
}

int EsifWebIsStarted()
{
	extern atomic_t g_ws_threads;
	return (atomic_read(&g_ws_threads) > 0);
}

void EsifWebSetIpaddrPort(const char *ipaddr, u32 port)
{
	esif_ws_server_set_ipaddr_port(ipaddr, port);
}

#else

eEsifError EsifWebStart()
{
	return ESIF_E_NOT_IMPLEMENTED;
}
void EsifWebStop()
{
}
int EsifWebIsStarted()
{
	return 0;
}
void EsifWebSetIpaddrPort(const char *ipaddr, u32 port)
{
	UNREFERENCED_PARAMETER(ipaddr);
	UNREFERENCED_PARAMETER(port);
}
#endif

eEsifError esif_uf_init()
{
	eEsifError rc = ESIF_OK;
	ESIF_TRACE_ENTRY_INFO();

#ifdef ESIF_ATTR_SHELL_LOCK
	esif_ccb_mutex_init(&g_shellLock);
#endif

	ESIF_TRACE_DEBUG("Init Upper Framework (UF)");

	esif_ccb_mempool_init_tracking();

	// Get Home directory
	CMD_OUT("Home: %s\n", esif_pathlist_get(ESIF_PATHTYPE_HOME));


	/* OS Agnostic */
	EsifLogMgrInit();

	esif_link_list_init();
	esif_ht_init();
	esif_ccb_tmrm_init();

	EsifCfgMgrInit();
	EsifEventMgr_Init();
	EsifCnjMgrInit();
	EsifUpPm_Init();
	EsifDspMgrInit();
	EsifActMgrInit();

	/* Web Server optionally started by shell scripts in esif_init */

	/* OS Specific */
	rc = esif_uf_os_init();
	if (ESIF_OK != rc) {
		goto exit;
	}

	/* Start App Manager after all dependent components started
	 * This does not actually start any apps.
	 */
	EsifAppMgrInit();

#ifdef ESIF_FEAT_OPT_ACTION_SYSFS
	SysfsRegisterParticipants();
#else
	ipc_connect();
	sync_lf_participants();
#endif

exit: 
	ESIF_TRACE_EXIT_INFO_W_STATUS(rc);
	return rc;
}


void esif_uf_exit()
{
	/* Stop event thread */
	g_quit = ESIF_TRUE;

	/* Stop web server if it is running */
	EsifWebStop();
	EsifDataLogStop();
	EsifUFPollStop();

	/* Stop all Apps before dependent components they may be using */
	EsifAppMgrExit();

	/* OS Specific */
	esif_uf_os_exit();

	/* OS Agnostic - Call in reverse order of Init */
	EsifUpPm_Exit();
	EsifActMgrExit();
	EsifDspMgrExit();
	EsifEventMgr_Exit();
	EsifCnjMgrExit();
	EsifCfgMgrExit();
	EsifLogMgrExit();
	esif_ccb_tmrm_exit();
	esif_ht_exit();
	esif_link_list_exit();
	esif_ccb_mempool_uninit_tracking();

	// Re-Initialize necessary global variables in case ESIF restarted
	g_esif_started = ESIF_FALSE;

	ESIF_TRACE_DEBUG("Exit Upper Framework (UF)");


#ifdef ESIF_ATTR_SHELL_LOCK
	esif_ccb_mutex_uninit(&g_shellLock);
#endif
	ESIF_TRACE_EXIT_INFO();
}

// Initialize at OS entry point
enum esif_rc esif_main_init(esif_string path_list)
{
	enum esif_rc rc = ESIF_OK;

#ifdef ESIF_ATTR_MEMTRACE
	esif_memtrace_init();	/* must be called first */
#endif

	esif_pathlist_init(path_list);

	// Return control to main OS entry point, which eventually calls esif_uf_init 
	return rc;
}

// Initialize at OS exit point
void esif_main_exit()
{
	// esif_uf_init and esif_uf_exit have already been called at this point

#ifdef ESIF_ATTR_MEMTRACE
	esif_memtrace_exit();	/* must be called last */
#else
	esif_pathlist_exit();	/* called by esif_memtrace_exit */
#endif
}

/* Memory Allocation Trace Support */
#ifdef ESIF_ATTR_MEMTRACE

void *esif_memtrace_alloc(
	void *old_ptr,
	size_t size,
	const char *func,
	const char *file,
	int line
	)
{
	struct memalloc_s *mem = NULL;
	struct memalloc_s **last = NULL;
	void *mem_ptr = NULL;

	esif_ccb_write_lock(&g_memtrace.lock);
	mem = g_memtrace.allocated;
	last = &g_memtrace.allocated;

	if (file) {
		const char *slash = strrchr(file, *ESIF_PATH_SEP);
		if (slash) {
			file = slash + 1;
		}
	}

	if (old_ptr) {
		mem_ptr = native_realloc(old_ptr, size);

		// realloc(ptr, size) leaves ptr unaffected if realloc fails and size is nonzero
		if (!mem_ptr && size > 0) {
			goto exit;
		}
		while (mem) {
			if (old_ptr == mem->mem_ptr) {
				// realloc(ptr, 0) behaves like free(ptr)
				if (size == 0) {
					*last = mem->next;
					native_free(mem);
				} else {
					mem->mem_ptr = mem_ptr;
					mem->size    = size;
					mem->func    = func;
					mem->file    = file;
					mem->line    = line;
				}
				goto exit;
			}
			last = &mem->next;
			mem = mem->next;
		}
	} else {
		mem_ptr = native_malloc(size);
		if (mem_ptr) {
			esif_ccb_memset(mem_ptr, 0, size);
			atomic_inc(&g_memtrace.allocs);
		}
	}

	mem = (struct memalloc_s *)native_malloc(sizeof(*mem));
	if (!mem) {
		goto exit;
	}
	esif_ccb_memset(mem, 0, sizeof(*mem));
	mem->mem_ptr = mem_ptr;
	mem->size    = size;
	mem->func    = func;
	mem->file    = file;
	mem->line    = line;
	mem->next    = g_memtrace.allocated;
	g_memtrace.allocated = mem;
exit:
	esif_ccb_write_unlock(&g_memtrace.lock);
	return mem_ptr;
}


char *esif_memtrace_strdup(
	const char *str,
	const char *func,
	const char *file,
	int line
	)
{
	size_t len    = esif_ccb_strlen(str, 0x7fffffff) + 1;
	char *mem_ptr = (char *)esif_memtrace_alloc(0, len, func, file, line);
	if (NULL != mem_ptr) {
		esif_ccb_strcpy(mem_ptr, str, len);
	}
	return mem_ptr;
}


void esif_memtrace_free(void *mem_ptr)
{
	struct memalloc_s *mem   = NULL;
	struct memalloc_s **last = NULL;

	esif_ccb_write_lock(&g_memtrace.lock);
	mem  = g_memtrace.allocated;
	last = &g_memtrace.allocated;

	while (mem) {
		if (mem_ptr == mem->mem_ptr) {
			*last = mem->next;
			native_free(mem);
			goto exit;
		}
		last = &mem->next;
		mem  = mem->next;
	}
exit:
	esif_ccb_write_unlock(&g_memtrace.lock);
	if (mem_ptr) {
		native_free(mem_ptr);
		atomic_inc(&g_memtrace.frees);
	}
}


void esif_memtrace_init()
{
	struct memalloc_s *mem = NULL;

	esif_ccb_lock_init(&g_memtrace.lock);
	esif_ccb_write_lock(&g_memtrace.lock);
	mem = g_memtrace.allocated;

	// Ignore any allocations made before this function was called
	while (mem) {
		struct memalloc_s *node = mem;
		mem = mem->next;
		native_free(node);
	}
	g_memtrace.allocated = NULL;
	esif_ccb_write_unlock(&g_memtrace.lock);
	ESIF_TRACE_EXIT_INFO();
}

void esif_memtrace_exit()
{
	struct memalloc_s *mem = NULL;
	char tracefile[MAX_PATH] = {0};
	FILE *tracelog = NULL;

	esif_build_path(tracefile, sizeof(tracefile), ESIF_PATHTYPE_LOG, "memtrace.txt", NULL);
	esif_pathlist_exit();

	esif_ccb_write_lock(&g_memtrace.lock);
	mem = g_memtrace.allocated;
	g_memtrace.allocated = NULL;
	esif_ccb_write_unlock(&g_memtrace.lock);

	CMD_OUT("MemTrace: Allocs=" ATOMIC_FMT " Frees=" ATOMIC_FMT "\n", atomic_read(&g_memtrace.allocs), atomic_read(&g_memtrace.frees));
	if (!mem) {
		goto exit;
	}

	CMD_OUT("\n*** MEMORY LEAKS DETECTED ***\nFor details see %s\n", tracefile);
	esif_ccb_fopen(&tracelog, tracefile, "a");
	if (tracelog) {
		time_t now = time(NULL);
		char timestamp[MAX_CTIME_LEN] = {0};
		esif_ccb_ctime(timestamp, sizeof(timestamp), &now);
		fprintf(tracelog, "\n*** %.24s: MEMORY LEAKS DETECTED (%s) ***\n", timestamp, ESIF_UF_VERSION);
	}
	while (mem) {
		struct memalloc_s *node = mem;
		if (tracelog) {
			fprintf(tracelog, "[%s @%s:%d]: (%lld bytes) %p\n", mem->func, mem->file, mem->line, (long long)mem->size, mem->mem_ptr);
		}
		mem = mem->next;
		native_free(node->mem_ptr);
		native_free(node);
	}
	if (tracelog) {
		esif_ccb_fclose(tracelog);
	}
exit:
	esif_ccb_lock_uninit(&g_memtrace.lock);
}


/************/

#endif /* ESIF_ATTR_MEMTRACE */

/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/

