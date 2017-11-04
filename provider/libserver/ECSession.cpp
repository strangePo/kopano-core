/*
 * Copyright 2005 - 2016 Zarafa and its licensors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <kopano/platform.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <poll.h>
#include <pwd.h>
#include <sys/stat.h>
#include <mapidefs.h>
#include <mapitags.h>
#include <kopano/lockhelper.hpp>
#include <kopano/UnixUtil.h>
#include <kopano/memory.hpp>
#include "ECSession.h"
#include "ECSessionManager.h"
#include "ECUserManagement.h"
#include "ECSecurity.h"
#include "ECPluginFactory.h"
#include "SSLUtil.h"
#include <kopano/stringutil.h>
#include "ECDatabase.h"
#include "ECDatabaseUtils.h" // used for PR_INSTANCE_KEY
#include "SOAPUtils.h"
#include "ics.h"
#include "ECICS.h"
#include <kopano/ECIConv.h>
#include "versions.h"
#if defined LINUX || !defined UNICODE
#define WHITESPACE " \t\n\r"
#else
#define WHITESPACE L" \t\n\r"
#endif

namespace KC {

// possible missing SSL function
#ifndef HAVE_EVP_PKEY_CMP
static int EVP_PKEY_cmp(EVP_PKEY *a, EVP_PKEY *b)
    {
    if (a->type != b->type)
        return -1;

    if (EVP_PKEY_cmp_parameters(a, b) == 0)
        return 0;

    switch (a->type)
        {
    case EVP_PKEY_RSA:
        if (BN_cmp(b->pkey.rsa->n,a->pkey.rsa->n) != 0
            || BN_cmp(b->pkey.rsa->e,a->pkey.rsa->e) != 0)
            return 0;
        break;
    case EVP_PKEY_DSA:
        if (BN_cmp(b->pkey.dsa->pub_key,a->pkey.dsa->pub_key) != 0)
            return 0;
        break;
    case EVP_PKEY_DH:
        return -2;
    default:
        return -2;
        }

    return 1;
    }
#endif

void CreateSessionID(unsigned int ulCapabilities, ECSESSIONID *lpSessionId)
{
	ssl_random(!!(ulCapabilities & KOPANO_CAP_LARGE_SESSIONID), lpSessionId);
}

/*
  BaseType session
*/
BTSession::BTSession(const char *src_addr, ECSESSIONID sessionID,
    ECDatabaseFactory *lpDatabaseFactory, ECSessionManager *lpSessionManager,
    unsigned int ulCapabilities) :
	m_strSourceAddr(src_addr), m_sessionID(sessionID),
	m_lpDatabaseFactory(lpDatabaseFactory),
	m_lpSessionManager(lpSessionManager),
	m_ulClientCapabilities(ulCapabilities)
{
	m_sessionTime = GetProcessTime();

	m_ulSessionTimeout = 300;
	m_bCheckIP = true;
	m_ulRequests = 0;

	m_ulLastRequestPort = 0;
}

void BTSession::SetClientMeta(const char *const lpstrClientVersion, const char *const lpstrClientMisc)
{
	m_strClientApplicationVersion = lpstrClientVersion ? lpstrClientVersion : "";
	m_strClientApplicationMisc = lpstrClientMisc ? lpstrClientMisc : "";
}

void BTSession::GetClientApplicationVersion(std::string *lpstrClientApplicationVersion)
{
        lpstrClientApplicationVersion->assign(m_strClientApplicationVersion);
}

void BTSession::GetClientApplicationMisc(std::string *lpstrClientApplicationMisc)
{
	scoped_lock lock(m_hRequestStats);
        lpstrClientApplicationMisc->assign(m_strClientApplicationMisc);
}

ECRESULT BTSession::Shutdown(unsigned int ulTimeout) {
	return erSuccess;
}

ECRESULT BTSession::ValidateOriginator(struct soap *soap)
{
	if (!m_bCheckIP)
		return erSuccess;
	const char *s = ::GetSourceAddr(soap);
	if (strcmp(m_strSourceAddr.c_str(), s) == 0)
		return erSuccess;
	ec_log_err("Denying access to session from source \"%s\" due to unmatched establishing source \"%s\"",
		s, m_strSourceAddr.c_str());
	return KCERR_END_OF_SESSION;
}

void BTSession::UpdateSessionTime()
{
	m_sessionTime = GetProcessTime();
}

ECRESULT BTSession::GetDatabase(ECDatabase **lppDatabase)
{
	return GetThreadLocalDatabase(this->m_lpDatabaseFactory, lppDatabase);
}

ECRESULT BTSession::GetAdditionalDatabase(ECDatabase **lppDatabase)
{
	std::string str;
	return this->m_lpDatabaseFactory->CreateDatabaseObject(lppDatabase, str);
}

ECRESULT BTSession::GetServerGUID(GUID* lpServerGuid){
	return 	m_lpSessionManager->GetServerGUID(lpServerGuid);
}

ECRESULT BTSession::GetNewSourceKey(SOURCEKEY* lpSourceKey){
	return m_lpSessionManager->GetNewSourceKey(lpSourceKey);
}

void BTSession::Lock()
{
	// Increase our refcount by one
	scoped_lock lock(m_hThreadReleasedMutex);
	++this->m_ulRefCount;
}

void BTSession::Unlock()
{
	// Decrease our refcount by one, signal ThreadReleased if RefCount == 0
	scoped_lock lock(m_hThreadReleasedMutex);
	--this->m_ulRefCount;
	if(!IsLocked())
		m_hThreadReleased.notify_one();
}

time_t BTSession::GetIdleTime()
{
	return GetProcessTime() - m_sessionTime;
}

void BTSession::RecordRequest(struct soap* soap)
{
	scoped_lock lock(m_hRequestStats);
	m_strLastRequestURL = soap->endpoint;
	m_ulLastRequestPort = soap->port;
	if (soap->proxy_from != nullptr && soap_info(soap)->bProxy)
		m_strProxyHost = soap->host;
	++m_ulRequests;
}

unsigned int BTSession::GetRequests()
{
	scoped_lock lock(m_hRequestStats);
    return m_ulRequests;
}

void BTSession::GetRequestURL(std::string *lpstrClientURL)
{
	scoped_lock lock(m_hRequestStats);
	lpstrClientURL->assign(m_strLastRequestURL);
}

void BTSession::GetProxyHost(std::string *lpstrProxyHost)
{
	scoped_lock lock(m_hRequestStats);
	lpstrProxyHost->assign(m_strProxyHost);
}

void BTSession::GetClientPort(unsigned int *lpulPort)
{
	scoped_lock lock(m_hRequestStats);
	*lpulPort = m_ulLastRequestPort;
}

size_t BTSession::GetInternalObjectSize()
{
	scoped_lock lock(m_hRequestStats);
	return MEMORY_USAGE_STRING(m_strSourceAddr) +
			MEMORY_USAGE_STRING(m_strLastRequestURL) +
			MEMORY_USAGE_STRING(m_strProxyHost);
}

ECSession::ECSession(const char *src_addr, ECSESSIONID sessionID,
    ECSESSIONGROUPID ecSessionGroupId, ECDatabaseFactory *lpDatabaseFactory,
    ECSessionManager *lpSessionManager, unsigned int ulCapabilities,
    AUTHMETHOD ulAuthMethod, int pid,
    const std::string &cl_ver, const std::string &cl_app,
    const std::string &cl_app_ver, const std::string &cl_app_misc) :
	BTSession(src_addr, sessionID, lpDatabaseFactory, lpSessionManager,
	    ulCapabilities),
	m_ulAuthMethod(ulAuthMethod), m_ulConnectingPid(pid),
	m_ecSessionGroupId(ecSessionGroupId), m_strClientVersion(cl_ver),
	m_ulClientVersion(KOPANO_VERSION_UNKNOWN), m_strClientApp(cl_app)
{
	m_lpTableManager.reset(new ECTableManager(this));
	m_strClientApplicationVersion   = cl_app_ver;
	m_strClientApplicationMisc	= cl_app_misc;

	ParseKopanoVersion(cl_ver, &m_ulClientVersion);
	// Ignore result.

	m_ulSessionTimeout = atoi(lpSessionManager->GetConfig()->GetSetting("session_timeout"));
	if (m_ulSessionTimeout < 300)
		m_ulSessionTimeout = 300;

	m_bCheckIP = strcmp(lpSessionManager->GetConfig()->GetSetting("session_ip_check"), "no") != 0;

	// Offline implements its own versions of these objects
	m_lpUserManagement.reset(new ECUserManagement(this, m_lpSessionManager->GetPluginFactory(), m_lpSessionManager->GetConfig()));
	m_lpEcSecurity.reset(new ECSecurity(this, m_lpSessionManager->GetConfig(), m_lpSessionManager->GetAudit()));

	// Atomically get and AddSession() on the sessiongroup. Needs a ReleaseSession() on the session group to clean up.
	m_lpSessionManager->GetSessionGroup(ecSessionGroupId, this, &m_lpSessionGroup);
}

ECSession::~ECSession()
{
	Shutdown(0);

	/*
	 * Release our reference to the session group; none of the threads of this session are
	 * using the object since there are now 0 threads on this session (except this thread)
	 * Afterwards tell the session manager that the sessiongroup may be an orphan now.
	 */
	if (m_lpSessionGroup) {
		m_lpSessionGroup->ReleaseSession(this);
    	m_lpSessionManager->DeleteIfOrphaned(m_lpSessionGroup);
	}
}

/**
 * Shut down the session:
 *
 * - Signal sessiongroup that long-running requests should be cancelled
 * - Wait for all users of the session to exit
 *
 * If the wait takes longer than ulTimeout milliseconds, KCERR_TIMEOUT is
 * returned. If this is the case, it is *not* safe to delete the session
 *
 * @param ulTimeout Timeout in milliseconds
 * @result erSuccess or KCERR_TIMEOUT
 */
ECRESULT ECSession::Shutdown(unsigned int ulTimeout)
{
	/* Shutdown blocking calls for this session on our session group */
	if (m_lpSessionGroup != nullptr)
		m_lpSessionGroup->ShutdownSession(this);

	/* Wait until there are no more running threads using this session */
	std::unique_lock<std::mutex> lk(m_hThreadReleasedMutex);
	while(IsLocked())
		if (m_hThreadReleased.wait_for(lk, std::chrono::milliseconds(ulTimeout)) == std::cv_status::timeout)
			break;
	lk.unlock();
	if (IsLocked())
		return KCERR_TIMEOUT;
	return erSuccess;
}

ECRESULT ECSession::AddAdvise(unsigned int ulConnection, unsigned int ulKey, unsigned int ulEventMask)
{
	ECRESULT		hr = erSuccess;

	Lock();

	if (m_lpSessionGroup)
		hr = m_lpSessionGroup->AddAdvise(m_sessionID, ulConnection, ulKey, ulEventMask);
	else
		hr = KCERR_NOT_INITIALIZED;

	Unlock();

	return hr;
}

ECRESULT ECSession::AddChangeAdvise(unsigned int ulConnection, notifySyncState *lpSyncState)
{
	ECRESULT		er = erSuccess;
	std::string strQuery;
	ECDatabase*		lpDatabase = NULL;
	DB_RESULT lpDBResult;
	DB_ROW			lpDBRow;
	ULONG			ulChangeId = 0;

	Lock();

	if (!m_lpSessionGroup) {
		er = KCERR_NOT_INITIALIZED;
		goto exit;
	}

	er = m_lpSessionGroup->AddChangeAdvise(m_sessionID, ulConnection, lpSyncState);
	if (er != hrSuccess)
		goto exit;

	er = GetDatabase(&lpDatabase);
	if (er != erSuccess)
		goto exit;

	strQuery =	"SELECT c.id FROM changes AS c JOIN syncs AS s "
					"ON s.sourcekey=c.parentsourcekey "
				"WHERE s.id=" + stringify(lpSyncState->ulSyncId) + " "
					"AND c.id>" + stringify(lpSyncState->ulChangeId) + " "
					"AND c.sourcesync!=" + stringify(lpSyncState->ulSyncId) + " "
					"AND c.change_type >=  " + stringify(ICS_MESSAGE) + " "
					"AND c.change_type & " + stringify(ICS_MESSAGE) + " !=  0 "
				"ORDER BY c.id DESC "
				"LIMIT 1";

	er = lpDatabase->DoSelect(strQuery, &lpDBResult);
	if (er != hrSuccess)
		goto exit;
	if (lpDBResult.get_num_rows() == 0)
		goto exit;
	lpDBRow = lpDBResult.fetch_row();
	if (lpDBRow == NULL || lpDBRow[0] == NULL) {
		er = KCERR_DATABASE_ERROR;
		ec_log_err("ECSession::AddChangeAdvise(): row or column null");
		goto exit;
	}

	ulChangeId = strtoul(lpDBRow[0], NULL, 0);
	er = m_lpSessionGroup->AddChangeNotification(m_sessionID, ulConnection, lpSyncState->ulSyncId, ulChangeId);

exit:
	Unlock();

	return er;
}

ECRESULT ECSession::DelAdvise(unsigned int ulConnection)
{
	ECRESULT hr = erSuccess;

	Lock();

	if (m_lpSessionGroup)
		hr = m_lpSessionGroup->DelAdvise(m_sessionID, ulConnection);
	else
		hr = KCERR_NOT_INITIALIZED;

	Unlock();

	return hr;
}

ECRESULT ECSession::AddNotificationTable(unsigned int ulType, unsigned int ulObjType, unsigned int ulTableId, sObjectTableKey* lpsChildRow, sObjectTableKey* lpsPrevRow, struct propValArray *lpRow)
{
	ECRESULT		hr = hrSuccess;

	Lock();

	if (m_lpSessionGroup)
		hr = m_lpSessionGroup->AddNotificationTable(m_sessionID, ulType, ulObjType, ulTableId, lpsChildRow, lpsPrevRow, lpRow);
	else
		hr = KCERR_NOT_INITIALIZED;

	Unlock();

	return hr;
}

ECRESULT ECSession::GetNotifyItems(struct soap *soap, struct notifyResponse *notifications)
{
	ECRESULT		hr = erSuccess;

	Lock();

	if (m_lpSessionGroup)
		hr = m_lpSessionGroup->GetNotifyItems(soap, m_sessionID, notifications);
	else
		hr = KCERR_NOT_INITIALIZED;

	Unlock();

	return hr;
}

void ECSession::AddBusyState(pthread_t threadId, const char *lpszState,
    const struct timespec &threadstart, double start)
{
	if (!lpszState) {		
		ec_log_err("Invalid argument \"lpszState\" in call to ECSession::AddBusyState()");
		return;
	}
	scoped_lock lock(m_hStateLock);
	m_mapBusyStates[threadId].fname = lpszState;
	m_mapBusyStates[threadId].threadstart = threadstart;
	m_mapBusyStates[threadId].start = start;
	m_mapBusyStates[threadId].threadid = threadId;
	m_mapBusyStates[threadId].state = SESSION_STATE_PROCESSING;
}

void ECSession::UpdateBusyState(pthread_t threadId, int state)
{
	scoped_lock lock(m_hStateLock);
	auto i = m_mapBusyStates.find(threadId);
	if (i != m_mapBusyStates.cend())
		i->second.state = state;
	else
		assert(false);
}

void ECSession::RemoveBusyState(pthread_t threadId)
{
	scoped_lock lock(m_hStateLock);

	auto i = m_mapBusyStates.find(threadId);
	if (i == m_mapBusyStates.cend()) {
		assert(false);
		return;
	}
	clockid_t clock;
	struct timespec end;

	// Since the specified thread is done now, record how much work it has done for us
	if(pthread_getcpuclockid(threadId, &clock) == 0) {
		clock_gettime(clock, &end);
		AddClocks(timespec2dbl(end) - timespec2dbl(i->second.threadstart), 0, GetTimeOfDay() - i->second.start);
	} else {
		assert(false);
	}
	m_mapBusyStates.erase(threadId);
}

void ECSession::GetBusyStates(std::list<BUSYSTATE> *lpStates)
{
	// this map is very small, since a session only performs one or two functions at a time
	// so the lock time is short, which will block _all_ incoming functions
	lpStates->clear();
	scoped_lock lock(m_hStateLock);
	for (const auto &p : m_mapBusyStates)
		lpStates->emplace_back(p.second);
}

void ECSession::AddClocks(double dblUser, double dblSystem, double dblReal)
{
	scoped_lock lock(m_hRequestStats);
	m_dblUser += dblUser;
	m_dblSystem += dblSystem;
	m_dblReal += dblReal;
}

void ECSession::GetClocks(double *lpdblUser, double *lpdblSystem, double *lpdblReal)
{
	scoped_lock lock(m_hRequestStats);
	*lpdblUser = m_dblUser;
	*lpdblSystem = m_dblSystem;
	*lpdblReal = m_dblReal;
}

void ECSession::GetClientVersion(std::string *lpstrVersion)
{
	scoped_lock lock(m_hRequestStats);
    lpstrVersion->assign(m_strClientVersion);
}

void ECSession::GetClientApp(std::string *lpstrClientApp)
{
	scoped_lock lock(m_hRequestStats);
    lpstrClientApp->assign(m_strClientApp);
}

/**
 * Get the object id of the object specified by the provided entryid.
 * This entryid can either be a short term or 'normal' entryid. If the entryid is a
 * short term entryid, the STE manager for this session will be queried for the object id.
 * If the entryid is a 'normal' entryid, the cache manager / database will be queried.
 *
 * @param[in]	lpEntryID		The entryid to get an object id for.
 * @param[out]	lpulObjId		Pointer to an unsigned int that will be set to the returned object id.
 * @param[out]	lpbIsShortTerm	Optional pointer to a boolean that will be set to true when the entryid
 * 								is a short term entryid.
 *
 * @retval	KCERR_INVALID_PARAMETER	lpEntryId or lpulObjId is NULL.
 * @retval	KCERR_INVALID_ENTRYID	The provided entryid is invalid.
 * @retval	KCERR_NOT_FOUND			No object was found for the provided entryid.
 */
ECRESULT ECSession::GetObjectFromEntryId(const entryId *lpEntryId, unsigned int *lpulObjId, unsigned int *lpulEidFlags)
{
	unsigned int ulObjId = 0;

	if (lpEntryId == NULL || lpulObjId == NULL)
		return KCERR_INVALID_PARAMETER;
	auto er = m_lpSessionManager->GetCacheManager()->GetObjectFromEntryId(lpEntryId, &ulObjId);
	if (er != erSuccess)
		return er;
	*lpulObjId = ulObjId;
	if (lpulEidFlags == NULL)
		return erSuccess;
	static_assert(offsetof(EID, usFlags) == offsetof(EID_V0, usFlags),
		"usFlags member not at same position");
	auto d = reinterpret_cast<EID *>(lpEntryId->__ptr);
	if (lpEntryId->__size < 0 ||
	    static_cast<size_t>(lpEntryId->__size) < offsetof(EID, usFlags) + sizeof(d->usFlags)) {
		ec_log_err("%s: entryid has size %d; not enough for EID_V1.usFlags",
			__func__, lpEntryId->__size);
		return MAPI_E_CORRUPT_DATA;
	}
	*lpulEidFlags = d->usFlags;
	return erSuccess;
}

ECRESULT ECSession::LockObject(unsigned int ulObjId)
{
	scoped_lock lock(m_hLocksLock);
	auto res = m_mapLocks.emplace(ulObjId, ECObjectLock());
	if (res.second == true)
		return m_lpSessionManager->GetLockManager()->LockObject(ulObjId, m_sessionID, &res.first->second);
	return erSuccess;
}

ECRESULT ECSession::UnlockObject(unsigned int ulObjId)
{
	scoped_lock lock(m_hLocksLock);

	auto i = m_mapLocks.find(ulObjId);
	if (i == m_mapLocks.cend())
		return erSuccess;
	auto er = i->second.Unlock();
	if (er == erSuccess)
		m_mapLocks.erase(i);
	return er;
}

size_t ECSession::GetObjectSize()
{
	size_t ulSize = sizeof(*this);

	ulSize += GetInternalObjectSize();
	ulSize += MEMORY_USAGE_STRING(m_strClientApp) +
			MEMORY_USAGE_STRING(m_strUsername) +
			MEMORY_USAGE_STRING(m_strClientVersion);

	ulSize += MEMORY_USAGE_MAP(m_mapBusyStates.size(), BusyStateMap);
	ulSize += MEMORY_USAGE_MAP(m_mapLocks.size(), LockMap);

	if (m_lpEcSecurity)
		ulSize += m_lpEcSecurity->GetObjectSize();

	// The Table manager size is not callculated here
//	ulSize += GetTableManager()->GetObjectSize();
	return ulSize;
}

ECAuthSession::ECAuthSession(const char *src_addr, ECSESSIONID sessionID,
    ECDatabaseFactory *lpDatabaseFactory, ECSessionManager *lpSessionManager,
    unsigned int ulCapabilities) :
	BTSession(src_addr, sessionID, lpDatabaseFactory, lpSessionManager,
	    ulCapabilities)
{
	m_ulSessionTimeout = 30;	// authenticate within 30 seconds, or else!
	m_lpUserManagement.reset(new ECUserManagement(this, m_lpSessionManager->GetPluginFactory(), m_lpSessionManager->GetConfig()));
#ifdef HAVE_GSSAPI
	m_gssServerCreds = GSS_C_NO_CREDENTIAL;
	m_gssContext = GSS_C_NO_CONTEXT;
#endif
}

ECAuthSession::~ECAuthSession()
{
#ifdef HAVE_GSSAPI
	OM_uint32 status;

	if (m_gssServerCreds)
		gss_release_cred(&status, &m_gssServerCreds);

	if (m_gssContext)
		gss_delete_sec_context(&status, &m_gssContext, GSS_C_NO_BUFFER);
#endif

	/* Wait until all locks have been closed */
	std::unique_lock<std::mutex> l_thread(m_hThreadReleasedMutex);
	m_hThreadReleased.wait(l_thread, [this](void) { return !IsLocked(); });
	l_thread.unlock();

	if (m_NTLM_pid != -1) {
		int status;

		// close I/O to make ntlm_auth exit
		close(m_stdin);
		close(m_stdout);
		close(m_stderr);

		// wait for process status
		waitpid(m_NTLM_pid, &status, 0);
		ec_log_info("Removing ntlm_auth on pid %d. Exitstatus: %d", m_NTLM_pid, status);
		if (status == -1) {
			ec_log_err(std::string("System call waitpid failed: ") + strerror(errno));
		} else {
#ifdef WEXITSTATUS
				if(WIFEXITED(status)) { /* Child exited by itself */
					if(WEXITSTATUS(status))
						ec_log_notice("ntlm_auth exited with non-zero status %d", WEXITSTATUS(status));
				} else if(WIFSIGNALED(status)) {        /* Child was killed by a signal */
					ec_log_err("ntlm_auth was killed by signal %d", WTERMSIG(status));

				} else {                        /* Something strange happened */
					ec_log_err("ntlm_auth terminated abnormally");
				}
#else
				if (status)
					ec_log_notice("ntlm_auth exited with status %d", status);
#endif
		}
	}
}

ECRESULT ECAuthSession::CreateECSession(ECSESSIONGROUPID ecSessionGroupId,
    const std::string &cl_ver, const std::string &cl_app,
    const std::string &cl_app_ver, const std::string &cl_app_misc,
    ECSESSIONID *sessionID, ECSession **lppNewSession)
{
	ECRESULT er = erSuccess;
	std::unique_ptr<ECSession> lpSession;
	ECSESSIONID newSID;

	if (!m_bValidated)
		return KCERR_LOGON_FAILED;
	CreateSessionID(m_ulClientCapabilities, &newSID);

	// ECAuthSessionOffline creates offline version .. no bOverrideClass construction
	lpSession.reset(new(std::nothrow) ECSession(m_strSourceAddr.c_str(),
	            newSID, ecSessionGroupId, m_lpDatabaseFactory,
	            m_lpSessionManager, m_ulClientCapabilities,
	            m_ulValidationMethod, m_ulConnectingPid,
	            cl_ver, cl_app, cl_app_ver, cl_app_misc));
	if (lpSession == nullptr)
		return KCERR_NOT_ENOUGH_MEMORY;
	er = lpSession->GetSecurity()->SetUserContext(m_ulUserID, m_ulImpersonatorID);
	if (er != erSuccess)
		/* User not found anymore, or error in getting groups. */
		return er;
	*sessionID = std::move(newSID);
	*lppNewSession = lpSession.release();
	return erSuccess;
}

// This is a standard user/pass login.
// You always log in as the user you are authenticating with.
ECRESULT ECAuthSession::ValidateUserLogon(const char* lpszName, const char* lpszPassword, const char* lpszImpersonateUser)
{
	ECRESULT er;

	if (!lpszName)
	{
		ec_log_err("Invalid argument \"lpszName\" in call to ECAuthSession::ValidateUserLogon()");
		return KCERR_INVALID_PARAMETER;
    }
	if (!lpszPassword) {
		ec_log_err("Invalid argument \"lpszPassword\" in call to ECAuthSession::ValidateUserLogon()");
		return KCERR_INVALID_PARAMETER;
	}

	// SYSTEM can't login with user/pass
	if (strcasecmp(lpszName, KOPANO_ACCOUNT_SYSTEM) == 0)
		return KCERR_NO_ACCESS;
	er = m_lpUserManagement->AuthUserAndSync(lpszName, lpszPassword, &m_ulUserID);
	if(er != erSuccess)
		return er;
	er = ProcessImpersonation(lpszImpersonateUser);
	if (er != erSuccess)
		return er;

	m_bValidated = true;
	m_ulValidationMethod = METHOD_USERPASSWORD;
	return erSuccess;
}

// Validate a user through the socket they are connecting through. This has the special feature
// that you can connect as a different user than you are specifying in the username. For example,
// you could be connecting as 'root' and being granted access because the kopano-server process
// is also running as 'root', but you are actually loggin in as user 'user1'.
ECRESULT ECAuthSession::ValidateUserSocket(int socket, const char* lpszName, const char* lpszImpersonateUser)
{
	ECRESULT 		er = erSuccess;
	const char *p = NULL;
	bool			allowLocalUsers = false;
	int				pid = 0;
	char			*ptr = NULL;
	std::unique_ptr<char, KCHL::cstdlib_deleter> localAdminUsers;

    if (!lpszName)
    {
		ec_log_err("Invalid argument \"lpszName\" in call to ECAuthSession::ValidateUserSocket()");
		return KCERR_INVALID_PARAMETER;
    }
	if (!lpszImpersonateUser) {
		ec_log_err("Invalid argument \"lpszImpersonateUser\" in call to ECAuthSession::ValidateUserSocket()");
		return KCERR_INVALID_PARAMETER;
	}
	p = m_lpSessionManager->GetConfig()->GetSetting("allow_local_users");
	if (p != nullptr && strcasecmp(p, "yes") == 0)
		allowLocalUsers = true;

	// Authentication stage
	localAdminUsers.reset(strdup(m_lpSessionManager->GetConfig()->GetSetting("local_admin_users")));

	struct passwd pwbuf;
	struct passwd *pw;
	uid_t uid;
	char strbuf[1024];
#ifdef SO_PEERCRED
	struct ucred cr;
	unsigned int cr_len;

	cr_len = sizeof(struct ucred);
	if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &cr, &cr_len) != 0 || cr_len != sizeof(struct ucred))
		return KCERR_LOGON_FAILED;

	uid = cr.uid; // uid is the uid of the user that is connecting
	pid = cr.pid;
#else // SO_PEERCRED
#ifdef HAVE_GETPEEREID
	gid_t gid;

	if (getpeereid(socket, &uid, &gid) != 0)
		return KCERR_LOGON_FAILED;
#else // HAVE_GETPEEREID
#error I have no way to find out the remote user and I want to cry
#endif // HAVE_GETPEEREID
#endif // SO_PEERCRED

	if (geteuid() == uid)
		// User connecting is connecting under same UID as the server is running under, allow this
		goto userok;

	// Lookup user name
	pw = NULL;
#ifdef HAVE_GETPWNAM_R
	getpwnam_r(lpszName, &pwbuf, strbuf, sizeof(strbuf), &pw);
#else
	// OpenBSD does not have getpwnam_r() .. FIXME: threading issue!
	pw = getpwnam(lpszName);
#endif

	if (allowLocalUsers && pw && pw->pw_uid == uid)
		// User connected as himself
		goto userok;

	p = strtok_r(localAdminUsers.get(), WHITESPACE, &ptr);

	while (p) {
	    pw = NULL;
#ifdef HAVE_GETPWNAM_R
		getpwnam_r(p, &pwbuf, strbuf, sizeof(strbuf), &pw);
#else
		pw = getpwnam(p);
#endif
		if (pw != nullptr && pw->pw_uid == uid)
			// A local admin user connected - ok
			goto userok;
		p = strtok_r(NULL, WHITESPACE, &ptr);
	}
	return KCERR_LOGON_FAILED;

userok:
    // Check whether user exists in the user database
	er = m_lpUserManagement->ResolveObjectAndSync(OBJECTCLASS_USER, lpszName, &m_ulUserID);
	if (er != erSuccess)
		return er;
	er = ProcessImpersonation(lpszImpersonateUser);
	if (er != erSuccess)
		return er;
	m_bValidated = true;
	m_ulValidationMethod = METHOD_SOCKET;
	m_ulConnectingPid = pid;
	return erSuccess;
}

ECRESULT ECAuthSession::ValidateUserCertificate(struct soap* soap, const char* lpszName, const char* lpszImpersonateUser)
{
	ECRESULT		er = KCERR_LOGON_FAILED;
	X509			*cert = NULL;			// client certificate
	EVP_PKEY		*pubkey = NULL;			// client public key
	EVP_PKEY		*storedkey = NULL;
	int				res = -1;

	const char *sslkeys_path = m_lpSessionManager->GetConfig()->GetSetting("sslkeys_path", "", NULL);
	std::unique_ptr<DIR, fs_deleter> dh;
	if (!soap) {
		ec_log_err("Invalid argument \"soap\" in call to ECAuthSession::ValidateUserCertificate()");
		return KCERR_INVALID_PARAMETER;
	}
	if (!lpszName) {
		ec_log_err("Invalid argument \"lpszName\" in call to ECAuthSession::ValidateUserCertificate()");
		return KCERR_INVALID_PARAMETER;
	}
	if (!lpszImpersonateUser) {
		ec_log_err("Invalid argument \"lpszImpersonateUser\" in call to ECAuthSession::ValidateUserCertificate()");
		return KCERR_INVALID_PARAMETER;
	}

	if (!sslkeys_path || sslkeys_path[0] == '\0') {
		ec_log_warn("No public keys directory defined in sslkeys_path.");
		return KCERR_LOGON_FAILED;
	}

	cert = SSL_get_peer_certificate(soap->ssl);
	if (!cert) {
		// Windows client without SSL certificate
		ec_log_info("No certificate in SSL connection.");
		return KCERR_LOGON_FAILED;
	}
	pubkey = X509_get_pubkey(cert);	// need to free
	if (!pubkey) {
		// if you get here, please tell me how, 'cause I'd like to know :)
		ec_log_info("No public key in certificate.");
		goto exit;
	}
	dh.reset(opendir(sslkeys_path));
	if (dh == nullptr) {
		ec_log_info("Cannot read directory \"%s\": %s", sslkeys_path, strerror(errno));
		er = KCERR_LOGON_FAILED;
		goto exit;
	}

	for (const struct dirent *dentry = readdir(dh.get());
	     dentry != nullptr; dentry = readdir(dh.get())) {
		const char *bname = dentry->d_name;
		auto fullpath = std::string(sslkeys_path) + "/" + bname;
		struct stat sb;

		if (stat(fullpath.c_str(), &sb) < 0 || !S_ISREG(sb.st_mode))
			continue;
		auto biofile = BIO_new_file(fullpath.c_str(), "r");
		if (!biofile) {
			ec_log_info("Unable to create BIO for \"%s\": %s", bname, ERR_error_string(ERR_get_error(), NULL));
			continue;
		}

		storedkey = PEM_read_bio_PUBKEY(biofile, NULL, NULL, NULL);
		if (!storedkey) {
			ec_log_info("Unable to read PUBKEY from \"%s\": %s", bname, ERR_error_string(ERR_get_error(), NULL));
			BIO_free(biofile);
			continue;
		}

		res = EVP_PKEY_cmp(pubkey, storedkey);

		BIO_free(biofile);
		EVP_PKEY_free(storedkey);

		if (res <= 0) {
			ec_log_info("Certificate \"%s\" does not match.", bname);
		} else {
			er = erSuccess;
			ec_log_info("Accepted certificate \"%s\" from client.", bname);
			break;
		}
	}
	if (er != erSuccess)
		goto exit;

    // Check whether user exists in the user database
	er = m_lpUserManagement->ResolveObjectAndSync(OBJECTCLASS_USER, lpszName, &m_ulUserID);
	if (er != erSuccess)
		goto exit;

	er = ProcessImpersonation(lpszImpersonateUser);
	if (er != erSuccess)
		goto exit;

	m_bValidated = true;
	m_ulValidationMethod = METHOD_SSL_CERT;

exit:
	if (cert)
		X509_free(cert);

	if (pubkey)
		EVP_PKEY_free(pubkey);

	return er;
}

#define NTLMBUFFER 8192
ECRESULT ECAuthSession::ValidateSSOData(struct soap* soap, const char* lpszName, const char* lpszImpersonateUser, const char* szClientVersion, const char *szClientApp, const char *szClientAppVersion, const char *szClientAppMisc, const struct xsd__base64Binary* lpInput, struct xsd__base64Binary **lppOutput)
{
	ECRESULT er = KCERR_INVALID_PARAMETER;
	if (!soap) {
		ec_log_err("Invalid argument \"soap\" in call to ECAuthSession::ValidateSSOData()");
		return er;
	}
	if (!lpszName) {
		ec_log_err("Invalid argument \"lpszName\" in call to ECAuthSession::ValidateSSOData()");
		return er;
	}
	if (!lpszImpersonateUser) {
		ec_log_err("Invalid argument \"lpszImpersonateUser\" in call to ECAuthSession::ValidateSSOData()");
		return er;
	}
	if (!szClientVersion) {
		ec_log_err("Invalid argument \"szClientVersion\" in call to ECAuthSession::ValidateSSOData()");
		return er;
	}
	if (!szClientApp) {
		ec_log_err("Invalid argument \"szClientApp\" in call to ECAuthSession::ValidateSSOData()");
		return er;
	}
	if (!lpInput) {
		ec_log_err("Invalid argument \"lpInput\" in call to ECAuthSession::ValidateSSOData()");
		return er;
	}
	if (!lppOutput) {
		ec_log_err("Invalid argument \"lppOutput\" in call to ECAuthSession::ValidateSSOData()");
		return er;
	}

	er = KCERR_LOGON_FAILED;

	// first NTLM package starts with that signature, continues are detected by the filedescriptor
	if (m_NTLM_pid != -1 || strncmp((const char*)lpInput->__ptr, "NTLM", 4) == 0)
		er = ValidateSSOData_NTLM(soap, lpszName, szClientVersion, szClientApp, szClientAppVersion, szClientAppMisc, lpInput, lppOutput);
	else
		er = ValidateSSOData_KRB5(soap, lpszName, szClientVersion, szClientApp, szClientAppVersion, szClientAppMisc, lpInput, lppOutput);
	if (er != erSuccess)
		return er;
	return ProcessImpersonation(lpszImpersonateUser);
}

#ifdef HAVE_GSSAPI
const char gss_display_status_fail_message[] = "Call to gss_display_status failed. Reason: ";

static ECRESULT LogKRB5Error_2(const char *msg, OM_uint32 code, OM_uint32 type)
{
	gss_buffer_desc gssMessage = GSS_C_EMPTY_BUFFER;
	OM_uint32 status = 0;
	OM_uint32 context = 0;

	if (msg == NULL) {
		ec_log_err("Invalid argument \"msg\" in call to ECAuthSession::LogKRB5Error()");
		return KCERR_INVALID_PARAMETER;
	}
	ECRESULT retval = KCERR_CALL_FAILED;
	do {
		OM_uint32 result = gss_display_status(&status, code, type, GSS_C_NULL_OID, &context, &gssMessage);
		switch (result) {
		case GSS_S_COMPLETE:
			ec_log_warn("%s: %s", msg, (char*)gssMessage.value);
			retval = erSuccess;
			break;
		case GSS_S_BAD_MECH:
			ec_log_warn("%s: %s", gss_display_status_fail_message, "unsupported mechanism type was requested.");
			retval = KCERR_CALL_FAILED;
			break;
		case GSS_S_BAD_STATUS:
			ec_log_warn("%s: %s", gss_display_status_fail_message, "status value was not recognized, or the status type was neither GSS_C_GSS_CODE nor GSS_C_MECH_CODE.");
			retval = KCERR_CALL_FAILED;
			break;
		}
		gss_release_buffer(&status, &gssMessage);
	} while (context != 0);
	return retval;
}

ECRESULT ECAuthSession::LogKRB5Error(const char* msg, OM_uint32 major, OM_uint32 minor)
{
	if (!msg) {
		ec_log_err("Invalid argument \"msg\" in call to ECAuthSession::LogKRB5Error()");
		return KCERR_INVALID_PARAMETER;
	}
	LogKRB5Error_2(msg, major, GSS_C_GSS_CODE);
	return LogKRB5Error_2(msg, minor, GSS_C_MECH_CODE);
}
#endif

ECRESULT ECAuthSession::ValidateSSOData_KRB5(struct soap* soap, const char* lpszName, const char* szClientVersion, const char* szClientApp, const char *szClientAppVersion, const char *szClientAppMisc, const struct xsd__base64Binary* lpInput, struct xsd__base64Binary** lppOutput)
{
	ECRESULT er = KCERR_INVALID_PARAMETER;
#ifndef HAVE_GSSAPI
	ec_log_err("Incoming Kerberos request, but this server was build without GSSAPI support.");
#else
	OM_uint32 retval, status;

	gss_name_t gssServername = GSS_C_NO_NAME;
	gss_buffer_desc gssInputBuffer = GSS_C_EMPTY_BUFFER;
	const char *szHostname = NULL;
	std::string principal;

	gss_name_t gssUsername = GSS_C_NO_NAME;
	gss_buffer_desc gssUserBuffer = GSS_C_EMPTY_BUFFER;
	gss_buffer_desc gssOutputToken = GSS_C_EMPTY_BUFFER;
	std::string strUsername;
	size_t pos;

	struct xsd__base64Binary *lpOutput = NULL;

	if (!soap) {
		ec_log_err("Invalid argument \"soap\" in call to ECAuthSession::ValidateSSOData_KRB5()");
		goto exit;
	}
	if (!lpszName) {
		ec_log_err("Invalid argument \"lpszName\" in call to ECAuthSession::ValidateSSOData_KRB5()");
		goto exit;
	}
	if (!szClientVersion) {
		ec_log_err("Invalid argument \"zClientVersionin\" in call to ECAuthSession::ValidateSSOData_KRB5()");
		goto exit;
	}
	if (!szClientApp) {
		ec_log_err("Invalid argument \"szClientApp\" in call to ECAuthSession::ValidateSSOData_KRB5()");
		goto exit;
	}
	if (!lpInput) {
		ec_log_err("Invalid argument \"lpInput\" in call to ECAuthSession::ValidateSSOData_KRB5()");
		goto exit;
	}
	if (!lppOutput) {
		ec_log_err("Invalid argument \"lppOutput\" in call to ECAuthSession::ValidateSSOData_KRB5()");
		goto exit;
	}
	er = KCERR_LOGON_FAILED;
	if (m_gssServerCreds == GSS_C_NO_CREDENTIAL) {
		m_gssContext = GSS_C_NO_CONTEXT;

		// ECServer made sure this setting option always contains the best hostname
		// If it's not there, that's unacceptable.
		szHostname = m_lpSessionManager->GetConfig()->GetSetting("server_hostname");
		if (!szHostname || szHostname[0] == '\0') {
			ec_log_crit("Hostname not found, required for Kerberos");
			goto exit;
		}
		principal = "kopano@";
		principal += szHostname;

		ec_log_debug("Kerberos principal: %s", principal.c_str());

		gssInputBuffer.value = (void*)principal.data();
		gssInputBuffer.length = principal.length() + 1;

		retval = gss_import_name(&status, &gssInputBuffer, GSS_C_NT_HOSTBASED_SERVICE, &gssServername);
		if (retval != GSS_S_COMPLETE) {
			LogKRB5Error("Unable to import server name", retval, status);
			goto exit;
		}

		retval = gss_acquire_cred(&status, gssServername, GSS_C_INDEFINITE, GSS_C_NO_OID_SET, GSS_C_ACCEPT, &m_gssServerCreds, NULL, NULL);
		if (retval != GSS_S_COMPLETE) {
			LogKRB5Error("Unable to acquire credentials handle", retval, status);
			goto exit;
		}
	}

	gssInputBuffer.length = lpInput->__size;
	gssInputBuffer.value = lpInput->__ptr;

	retval = gss_accept_sec_context(&status, &m_gssContext, m_gssServerCreds, &gssInputBuffer, GSS_C_NO_CHANNEL_BINDINGS, &gssUsername, NULL, &gssOutputToken, NULL, NULL, NULL);

	if (gssOutputToken.length) {
		// we need to send data back to the client, no need to consider retval
		lpOutput = s_alloc<struct xsd__base64Binary>(soap);
		lpOutput->__size = gssOutputToken.length;
		lpOutput->__ptr = s_alloc<unsigned char>(soap, gssOutputToken.length);
		memcpy(lpOutput->__ptr, gssOutputToken.value, gssOutputToken.length);

		gss_release_buffer(&status, &gssOutputToken);
	}

	if (retval == GSS_S_CONTINUE_NEEDED) {
		er = KCERR_SSO_CONTINUE;
		goto exit;
	} else if (retval != GSS_S_COMPLETE) {
		LogKRB5Error("Unable to accept security context", retval, status);
		ZLOG_AUDIT(m_lpSessionManager->GetAudit(), "authenticate failed user='%s' from='%s' method='kerberos sso' program='%s'",
			lpszName, soap->host, szClientApp);
		goto exit;
	}

	retval = gss_display_name(&status, gssUsername, &gssUserBuffer, NULL);
	if (retval) {
		LogKRB5Error("Unable to convert username", retval, status);
		goto exit;
	}

	ec_log_debug("Kerberos username: %s", static_cast<const char *>(gssUserBuffer.value));
	// kerberos returns: username@REALM, username is case-insensitive
	strUsername.assign((char*)gssUserBuffer.value, gssUserBuffer.length);
	pos = strUsername.find_first_of('@');
	if (pos != std::string::npos)
		strUsername.erase(pos);

	if (strcasecmp(strUsername.c_str(), lpszName) == 0) {
		er = m_lpUserManagement->ResolveObjectAndSync(ACTIVE_USER, lpszName, &m_ulUserID);
		// don't check NONACTIVE, since those shouldn't be able to login
		if(er != erSuccess)
			goto exit;

		m_bValidated = true;
		m_ulValidationMethod = METHOD_SSO;
		ec_log_info("Kerberos Single Sign-On: User \"%s\" authenticated", lpszName);
		ZLOG_AUDIT(m_lpSessionManager->GetAudit(), "authenticate ok user='%s' from='%s' method='kerberos sso' program='%s'",
			lpszName, soap->host, szClientApp);
	} else {
		ec_log_err("Kerberos username \"%s\" authenticated, but user \"%s\" requested.", (char*)gssUserBuffer.value, lpszName);
		ZLOG_AUDIT(m_lpSessionManager->GetAudit(), "authenticate spoofed user='%s' requested='%s' from='%s' method='kerberos sso' program='%s'",
			static_cast<char *>(gssUserBuffer.value), lpszName, soap->host, szClientApp);
	}

exit:
	if (gssUserBuffer.length)
		gss_release_buffer(&status, &gssUserBuffer);

	if (gssOutputToken.length)
		gss_release_buffer(&status, &gssOutputToken);

	if (gssUsername != GSS_C_NO_NAME)
		gss_release_name(&status, &gssUsername);

	if (gssServername != GSS_C_NO_NAME)
		gss_release_name(&status, &gssServername);
	if (lppOutput != nullptr)
		*lppOutput = lpOutput;
#endif

	return er;
}

ECRESULT ECAuthSession::ValidateSSOData_NTLM(struct soap* soap, const char* lpszName, const char* szClientVersion, const char* szClientApp, const char *szClientAppVersion, const char *szClientAppMisc, const struct xsd__base64Binary* lpInput, struct xsd__base64Binary **lppOutput)
{
	ECRESULT er = KCERR_INVALID_PARAMETER;
	struct xsd__base64Binary *lpOutput = NULL;
	char buffer[NTLMBUFFER];
	std::string strEncoded, strDecoded, strAnswer;
	ssize_t bytes = 0;
	char separator = '\\';      // get config version
	struct pollfd pollfd[2] = {{m_stdout, POLLIN | POLLRDHUP}, {m_stderr, POLLIN | POLLRDHUP}};

	if (!soap) {
		ec_log_err("Invalid argument \"soap\" in call to ECAuthSession::ValidateSSOData_NTLM()");
		return er;
	}
	if (!lpszName) {
		ec_log_err("Invalid argument \"lpszName\" in call to ECAuthSession::ValidateSSOData_NTLM()");
		return er;
	}
	if (!szClientVersion) {
		ec_log_err("Invalid argument \"zClientVersionin\" in call to ECAuthSession::ValidateSSOData_NTLM()");
		return er;
	}
	if (!szClientApp) {
		ec_log_err("Invalid argument \"szClientApp\" in call to ECAuthSession::ValidateSSOData_NTLM()");
		return er;
	}
	if (!lpInput) {
		ec_log_err("Invalid argument \"lpInput\" in call to ECAuthSession::ValidateSSOData_NTLM()");
		return er;
	}
	if (!lppOutput) {
		ec_log_err("Invalid argument \"lppOutput\" in call to ECAuthSession::ValidateSSOData_NTLM()");
		return er;
	}
	er = KCERR_LOGON_FAILED;
	strEncoded = base64_encode(lpInput->__ptr, lpInput->__size);
	errno = 0;

	if (m_NTLM_pid == -1) {
		// start new ntlmauth pipe
		// TODO: configurable path?

		if (pipe(m_NTLM_stdin) == -1 || pipe(m_NTLM_stdout) == -1 || pipe(m_NTLM_stderr) == -1) {
			ec_log_crit(std::string("Unable to create communication pipes for ntlm_auth: ") + strerror(errno));
			return er;
		}

		/*
		 * Why are we using vfork() ?
		 *
		 * You might as well use fork() here but vfork() is much faster in our case; this is because vfork() doesn't actually duplicate
		 * any pages, expecting you to call execl(). Watch out though, since data changes done in the client process before execl() WILL
		 * affect the mother process. (however, the file descriptor table is correctly cloned)
		 *
		 * The reason fork() is slow is that even though it is doing a Copy-On-Write copy, it still needs to do some page-copying to set up your
		 * process. This copying time increases with memory usage of the mother process; in fact, running 200 forks() on a process occupying
		 * 512MB of memory takes 15 seconds, while the same vfork()/exec() loop takes under .5 of a second.
		 *
		 * If vfork() is not available, or is broken on another platform, it is safe to simply replace it with fork(), but it will be quite slow!
		 */

		m_NTLM_pid = vfork();
		if (m_NTLM_pid == -1) {
			// broken
			ec_log_crit(std::string("Unable to start new process for ntlm_auth: ") + strerror(errno));
			return er;
		} else if (m_NTLM_pid == 0) {
			// client
			int j, k;

			close(m_NTLM_stdin[1]);
			close(m_NTLM_stdout[0]);
			close(m_NTLM_stderr[0]);

			dup2(m_NTLM_stdin[0], 0);
			dup2(m_NTLM_stdout[1], 1);
			dup2(m_NTLM_stderr[1], 2);

			// close all other open file descriptors, so ntlm doesn't keep the kopano-server sockets open
			j = getdtablesize();
			for (k = 3; k < j; ++k)
				close(k);

			execl("/bin/sh", "sh", "-c", "ntlm_auth -d0 --helper-protocol=squid-2.5-ntlmssp", NULL);

			ec_log_crit(std::string("Cannot start ntlm_auth: ") + strerror(errno));
			_exit(2);
		} else {
			// parent
			ec_log_info("New ntlm_auth started on pid %d", m_NTLM_pid);
			close(m_NTLM_stdin[0]);
			close(m_NTLM_stdout[1]);
			close(m_NTLM_stderr[1]);
			m_stdin = ec_relocate_fd(m_NTLM_stdin[1]);
			m_stdout = ec_relocate_fd(m_NTLM_stdout[0]);
			m_stderr = ec_relocate_fd(m_NTLM_stderr[0]);

			// Yo! Refresh!
			write(m_stdin, "YR ", 3);
			write(m_stdin, strEncoded.c_str(), strEncoded.length());
			write(m_stdin, "\n", 1);
		}
	} else {
		// Knock knock! who's there?
		write(m_stdin, "KK ", 3);
		write(m_stdin, strEncoded.c_str(), strEncoded.length());
		write(m_stdin, "\n", 1);
	}

	memset(buffer, 0, NTLMBUFFER);

retry:
	pollfd[0].revents = pollfd[1].revents = 0;
	int ret = poll(pollfd, 2, 10 * 1000); // timeout of 10 seconds before ntlm_auth can respond too large?
	if (ret < 0) {
		if (errno == EINTR)
			goto retry;
		ec_log_err(std::string("Error while waiting for data from ntlm_auth: ") + strerror(errno));
		return er;
	}

	if (ret == 0) {
		// timeout
		ec_log_err("Timeout while reading from ntlm_auth");
		return er;
	}

	// stderr is optional, and always written first
	if (pollfd[1].revents & (POLLIN | POLLRDHUP)) {
		// log stderr of ntlm_auth to logfile (loop?)
		bytes = read(m_stderr, buffer, NTLMBUFFER-1);
		if (bytes < 0)
			return er;
		buffer[bytes] = '\0';
		// print in lower level. if ntlm_auth was not installed (kerberos only environment), you won't care that ntlm_auth doesn't work.
		// login error is returned to the client, which was expected anyway.
		ec_log_notice(std::string("Received error from ntlm_auth:\n") + buffer);
		return er;
	}

	// stdout is mandatory, so always read from this pipe
	memset(buffer, 0, NTLMBUFFER);
	bytes = read(m_stdout, buffer, NTLMBUFFER-1);
	if (bytes < 0) {
		ec_log_err(std::string("Unable to read data from ntlm_auth: ") + strerror(errno));
		return er;
	} else if (bytes == 0) {
		ec_log_err("Nothing read from ntlm_auth");
		return er;
	}
	if (buffer[bytes-1] == '\n')
		/*
		 * Strip newline right away, it is not useful for logging,
		 * nor for base64_decode.
		 */
		buffer[--bytes] = '\0';
	if (bytes < 2) {
		/* Ensure buffer[0]==.. && buffer[1]==.. is valid to do */
		ec_log_err("Short reply from ntlm_auth");
		return er;
	}

	if (bytes >= 3)
		/*
		 * Extract response text (if any) following the reply code
		 * (and space). Else left empty.
		 */
		strAnswer.assign(buffer + 3, bytes - 3);

	if (buffer[0] == 'B' && buffer[1] == 'H') {
		/*
		 * "Broken Helper". Either we fed nonsensical data to ntlm_auth
		 * (unlikely), or ntlm_auth found some reason not to complete,
		 * like /var/lib/samba/winbindd_privileged being inaccessible.
		 */
		ec_log_err("ntlm_auth returned generic error \"%.*s\"", static_cast<int>(bytes), buffer);
		return er;
	} else if (buffer[0] == 'T' && buffer[1] == 'T') {
		// Try This
		strDecoded = base64_decode(strAnswer);

		lpOutput = s_alloc<struct xsd__base64Binary>(soap);
		lpOutput->__size = strDecoded.length();
		lpOutput->__ptr = s_alloc<unsigned char>(soap, strDecoded.length());
		memcpy(lpOutput->__ptr, strDecoded.data(), strDecoded.length());

		er = KCERR_SSO_CONTINUE;

	} else if (buffer[0] == 'A' && buffer[1] == 'F') {
		// Authentication Fine
		// Samba default runs in UTF-8 and setting 'unix charset' to windows-1252 in the samba config will break ntlm_auth
		// convert the username before we use it in Kopano
		ECIConv iconv("windows-1252", "utf-8");
		if (!iconv.canConvert()) {
			ec_log_crit("Problem setting up windows-1252 to utf-8 converter");
			return er;
		}

		strAnswer = iconv.convert(strAnswer);

		ec_log_info("Found username (%s)", strAnswer.c_str());

		// if the domain separator is not found, assume we only have the username (samba)
		auto pos = strAnswer.find_first_of(separator);
		if (pos != std::string::npos) {
			++pos;
			strAnswer.assign(strAnswer, pos, strAnswer.length()-pos);
		}

		// Check whether user exists in the user database
		er = m_lpUserManagement->ResolveObjectAndSync(ACTIVE_USER, (char *)strAnswer.c_str(), &m_ulUserID);
		// don't check NONACTIVE, since those shouldn't be able to login
		if(er != erSuccess)
			return er;

		if (strcasecmp(lpszName, strAnswer.c_str()) != 0) {
			// cannot open another user without password
			// or should we check permissions ?
			ec_log_warn("Single Sign-On: User \"%s\" authenticated, but user \"%s\" requested.", strAnswer.c_str(), lpszName);
			ZLOG_AUDIT(m_lpSessionManager->GetAudit(), "authenticate spoofed user='%s' requested='%s' from='%s' method='ntlm sso' program='%s'",
				strAnswer.c_str(), lpszName, soap->host, szClientApp);
			er = KCERR_LOGON_FAILED;
		} else {
			m_bValidated = true;
			m_ulValidationMethod = METHOD_SSO;
			er = erSuccess;
			ec_log_info("Single Sign-On: User \"%s\" authenticated", strAnswer.c_str());
			ZLOG_AUDIT(m_lpSessionManager->GetAudit(), "authenticate ok user='%s' from='%s' method='ntlm sso' program='%s'",
				lpszName, soap->host, szClientApp);
		}

	} else if (buffer[0] == 'N' && buffer[1] == 'A') {
		// Not Authenticated
		ec_log_info("Requested user \"%s\" denied. Not authenticated: \"%s\"", lpszName, strAnswer.c_str());
		ZLOG_AUDIT(m_lpSessionManager->GetAudit(), "authenticate failed user='%s' from='%s' method='ntlm sso' program='%s'",
			lpszName, soap->host, szClientApp);
		er = KCERR_LOGON_FAILED;
	} else {
		// unknown response?
		ec_log_err("Unknown response from ntlm_auth: %.*s", static_cast<int>(bytes), buffer);
		return KCERR_CALL_FAILED;
	}

	*lppOutput = lpOutput;
	return er;
}
#undef NTLMBUFFER

ECRESULT ECAuthSession::ProcessImpersonation(const char* lpszImpersonateUser)
{
	if (lpszImpersonateUser == NULL || *lpszImpersonateUser == '\0') {
		m_ulImpersonatorID = EC_NO_IMPERSONATOR;
		return erSuccess;
	}

	m_ulImpersonatorID = m_ulUserID;
	return m_lpUserManagement->ResolveObjectAndSync(OBJECTCLASS_USER,
	       lpszImpersonateUser, &m_ulUserID);
}

size_t ECAuthSession::GetObjectSize()
{
	return sizeof(*this);
}

} /* namespace */
