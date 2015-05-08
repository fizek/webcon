#include "extension.h"

#include "CDetour/detours.h"

#include "microhttpd.h"

#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#define closesocket close
#define ioctlsocket ioctl
#define WSAGetLastError() errno
#endif

// tier1 supremecy
#include "utlvector.h"
#include "netadr.h"

Webcon g_Webcon;
SMEXT_LINK(&g_Webcon);

IGameConfig *gameConfig;

bool shouldHandleProcessAccept;

CDetour *detourProcessAccept;
CDetour *detourRunFrame;

MHD_Daemon *httpDaemon;

MHD_Response *responseNotFound;
MHD_Response *responseInternalServerError;

struct PluginRequestHandler
{
	PluginRequestHandler(IPluginContext *context, funcid_t function, const char *name, const char *description);
	~PluginRequestHandler();

	bool Execute(MHD_Connection *connection, const char *method, const char *url);

	IChangeableForward *callback;
	char *name;
	char *description;
};

PluginRequestHandler::PluginRequestHandler(IPluginContext *context, funcid_t function, const char *name, const char *description) {
	callback = forwards->CreateForwardEx(NULL, ET_Single, 3, NULL, Param_Cell, Param_String, Param_String);
	callback->AddFunction(context, function);

	this->name = strdup(name);
	this->description = strdup(description);
}

PluginRequestHandler::~PluginRequestHandler() {
	forwards->ReleaseForward(callback);

	free(name);
	free(description);
}

bool PluginRequestHandler::Execute(MHD_Connection *connection, const char *method, const char *url) {
	Handle_t handle = (Handle_t)(MHD_get_connection_info(connection, MHD_CONNECTION_INFO_SOCKET_CONTEXT)->socket_context);

	if (handle == BAD_HANDLE) {
		return false;
	}

	callback->PushCell(handle);
	callback->PushString(method);
	callback->PushString(url);

	cell_t result;
	callback->Execute(&result);

	return (result != 0);
}

PluginRequestHandler *defaultHandler;

struct PendingSocket
{
	int timeout;
	int socket;
	sockaddr socketAddress;
	socklen_t socketAddressLength;
	netadr_t address;
};

CUtlVector<PendingSocket> pendingSockets;

struct ISocketCreatorListener
{
	virtual bool ShouldAcceptSocket(int socket, const netadr_t &address) = 0; 
	virtual void OnSocketAccepted(int socket, const netadr_t &address, void **data) = 0; 
	virtual void OnSocketClosed(int socket, const netadr_t &address, void *data) = 0;
};

struct CRConServer: public ISocketCreatorListener
{
	static void *HandleFailedRconAuthFunction;
	bool HandleFailedRconAuth(const netadr_t &address);
};

CRConServer *rconServer;

void *CRConServer::HandleFailedRconAuthFunction = NULL;

bool CRConServer::HandleFailedRconAuth(const netadr_t &address)
{
	if (!CRConServer::HandleFailedRconAuthFunction) {
		return false;
	}

#ifdef _WIN32
	return ((bool (__fastcall *)(CRConServer *, void *, const netadr_t &))CRConServer::HandleFailedRconAuthFunction)(this, NULL, address);
#else
	return ((bool (*)(CRConServer *, const netadr_t &))CRConServer::HandleFailedRconAuthFunction)(this, address);
#endif
}

struct CSocketCreator 
{
	// These are our own functions, they're in here for convenient access to the engine's CSocketCreator variables.
	void ProcessAccept();
	void HandSocketToEngine(PendingSocket *pendingSocket);

	struct AcceptedSocket
	{
		int socket;
		netadr_t address;
		void *data;
	};

	ISocketCreatorListener *listener;
	CUtlVector<AcceptedSocket> acceptedSockets;
	int listenSocket;
	netadr_t listenAddress;
};

void CSocketCreator::ProcessAccept()
{
	sockaddr socketAddress;
	socklen_t socketAddressLength = sizeof(socketAddress);
	int socket = accept(listenSocket, &socketAddress, &socketAddressLength);
	if (socket == -1) {
		return;
	}

	rootconsole->ConsolePrint("(%d) New listen socket accepted.", socket);

	int opt = 1;
	setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char *)&opt, sizeof(opt)); 

	opt = 1;
	setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

	opt = 1;
	if (ioctlsocket(socket, FIONBIO, (unsigned long *)&opt) == -1) {
		rootconsole->ConsolePrint("(%d) Failed to set socket options.", socket);
		closesocket(socket);
		return;
	}

	netadr_t address;
	address.SetFromSockadr(&socketAddress);

	if (listener && !listener->ShouldAcceptSocket(socket, address)) {
		rootconsole->ConsolePrint("(%d) Listener rejected connection.", socket);
		closesocket(socket);
		return;
	}

	PendingSocket *pendingSocket = &pendingSockets[pendingSockets.AddToTail()];
	pendingSocket->timeout = 0;
	pendingSocket->socket = socket;
	pendingSocket->socketAddress = socketAddress;
	pendingSocket->socketAddressLength = socketAddressLength;
	pendingSocket->address = address;
}

void CSocketCreator::HandSocketToEngine(PendingSocket *pendingSocket)
{
	AcceptedSocket *acceptedSocket = &acceptedSockets[acceptedSockets.AddToTail()];
	acceptedSocket->socket = pendingSocket->socket;
	acceptedSocket->address = pendingSocket->address;
	acceptedSocket->data = NULL;

	if (listener) {
		listener->OnSocketAccepted(acceptedSocket->socket, acceptedSocket->address, &(acceptedSocket->data));
	}
}

bool SocketWouldBlock() {
#if _WIN32
	return (WSAGetLastError() == WSAEWOULDBLOCK);
#else
	return (errno == EAGAIN || errno == EWOULDBLOCK);
#endif
}

DETOUR_DECL_MEMBER0(ProcessAccept, void)
{
	if (!shouldHandleProcessAccept) {
		return DETOUR_MEMBER_CALL(ProcessAccept)();
	}

	CSocketCreator *creator = (CSocketCreator *)this;

	// Check for incoming sockets first.
	creator->ProcessAccept();

	// Just enough to verify if it is RCON or HTTP(S).
	unsigned char buffer[12];

	int count = pendingSockets.Count();
	for (int i = (count - 1); i >= 0; --i) {
		PendingSocket *pendingSocket = &pendingSockets[i];

		ssize_t ret = recv(pendingSocket->socket, (char *)buffer, sizeof(buffer), MSG_PEEK);

		if (ret == 0) {
			rootconsole->ConsolePrint("(%d) Listen socket closed.", pendingSocket->socket);
			closesocket(pendingSocket->socket);

			pendingSockets.Remove(i);
			continue;
		}

		if (ret == -1 && !SocketWouldBlock()) {
			rootconsole->ConsolePrint("(%d) recv error: %d", WSAGetLastError());
			closesocket(pendingSocket->socket);

			pendingSockets.Remove(i);
			continue;
		}

		// We need at least n bytes to identify packets.
		if (ret < (ssize_t)sizeof(buffer)) {
			pendingSocket->timeout++;

			// About 15 seconds.
			if (pendingSocket->timeout > 1000) {
				if (rconServer) {
					rconServer->HandleFailedRconAuth(pendingSocket->address);
				}

				rootconsole->ConsolePrint("(%d) Listen socket timed out.", pendingSocket->socket);
				closesocket(pendingSocket->socket);

				pendingSockets.Remove(i);
			}

			continue;
		}

#if 0
		META_CONPRINTF("(%d) Packet Header:", pendingSocket->socket);
		for (unsigned j = 0; j < sizeof(buffer); ++j) {
			META_CONPRINTF(" %02X", buffer[j]);
		}
		META_CONPRINTF("\n");
#endif

		bool isHttp = ((buffer[0] >= 'A' && buffer[0] <= 'Z') || (buffer[0] >= 'a' && buffer[0] <= 'z')) &&
		              ((buffer[1] >= 'A' && buffer[1] <= 'Z') || (buffer[1] >= 'a' && buffer[1] <= 'z')) &&
		              ((buffer[2] >= 'A' && buffer[2] <= 'Z') || (buffer[2] >= 'a' && buffer[2] <= 'z'));

		bool isHttps = buffer[0] == 0x16 && buffer[1] == 0x03 && buffer[5] == 0x01 && buffer[6] == 0x00 &&
		               ((buffer[3] * 256) + buffer[4]) == ((buffer[7] * 256) + buffer[8] + 4);

		bool isRcon = buffer[2] == 0x00 && buffer[3] == 0x00 &&
		              (buffer[8] == 0x03 && buffer[9] == 0x00 && buffer[10] == 0x00 && buffer[11] == 0x00);

		if (isHttp || isHttps) {
			MHD_add_connection(httpDaemon, pendingSocket->socket, &(pendingSocket->socketAddress), pendingSocket->socketAddressLength);
			rootconsole->ConsolePrint("(%d) Gave %s socket to web server.", pendingSocket->socket, isHttps ? "HTTPS" : "HTTP");
		} else if (isRcon) {
			creator->HandSocketToEngine(pendingSocket);
			rootconsole->ConsolePrint("(%d) Gave RCON socket to engine.", pendingSocket->socket);
		} else {
			if (rconServer) {
				rconServer->HandleFailedRconAuth(pendingSocket->address);
			}

			rootconsole->ConsolePrint("(%d) Unidentified protocol on socket.", pendingSocket->socket);
			closesocket(pendingSocket->socket);
		}

		pendingSockets.Remove(i);
	}

	// Now everyone has their sockets, do HTTP work.
	MHD_run(httpDaemon);
}

DETOUR_DECL_MEMBER0(RunFrame, void)
{
	rconServer = (CRConServer *)this;

	shouldHandleProcessAccept = true;
	DETOUR_MEMBER_CALL(RunFrame)();
	shouldHandleProcessAccept = false;
}

HandleType_t handleTypeResponse;

struct ResponseTypeHandler: public IHandleTypeDispatch
{
	void OnHandleDestroy(HandleType_t type, void *object);
};

ResponseTypeHandler handlerResponseType;

void ResponseTypeHandler::OnHandleDestroy(HandleType_t type, void *object)
{
	MHD_destroy_response((MHD_Response *)object);
}

HandleType_t handleTypeConnection;

struct ConnectionTypeHandler: public IHandleTypeDispatch
{
	void OnHandleDestroy(HandleType_t type, void *object);
};

ConnectionTypeHandler handlerConnectionType;

void ConnectionTypeHandler::OnHandleDestroy(HandleType_t type, void *object)
{
	// Do nothing.
}

cell_t WebResponse_AddHeader(IPluginContext *context, const cell_t *params)
{
	HandleSecurity security;
	security.pOwner = context->GetIdentity();
	security.pIdentity = myself->GetIdentity();

	MHD_Response *response;
	HandleError error = handlesys->ReadHandle(params[1], handleTypeResponse, &security, (void **)&response);
	if (error != HandleError_None) {
		return context->ThrowNativeError("Invalid response handle %x (error %d)", params[1], error);
	}

	char *header;
	context->LocalToString(params[2], &header);

	char *content;
	context->LocalToString(params[3], &content);

	return MHD_add_response_header(response, header, content);
}

cell_t WebStringResponse_WebStringResponse(IPluginContext *context, const cell_t *params)
{
	char *content;
	context->LocalToString(params[1], &content);

	MHD_Response *response = MHD_create_response_from_buffer(strlen(content), (void *)content, MHD_RESPMEM_MUST_COPY);

	return handlesys->CreateHandle(handleTypeResponse, response, NULL, myself->GetIdentity(), NULL);
}

cell_t WebBinaryResponse_WebBinaryResponse(IPluginContext *context, const cell_t *params)
{
	char *content;
	context->LocalToString(params[1], &content);

	MHD_Response *response = MHD_create_response_from_buffer(params[2], (void *)content, MHD_RESPMEM_MUST_COPY);

	return handlesys->CreateHandle(handleTypeResponse, response, NULL, myself->GetIdentity(), NULL);
}

cell_t WebFileResponse_WebFileResponse(IPluginContext *context, const cell_t *params)
{
	char *path;
	context->LocalToString(params[1], &path);

	char realPath[PLATFORM_MAX_PATH];
	smutils->BuildPath(Path_Game, realPath, sizeof(realPath), "%s", path);

#ifdef _WIN32
	int fd = _open(realPath, _O_RDONLY | _O_BINARY | _O_SEQUENTIAL);
#else
	int fd = open(realPath, O_RDONLY);
#endif

	if (fd == -1) {
		return context->ThrowNativeError("Failed to open \"%s\" (error %d)", path, errno);
	}

#ifdef _WIN32
	long size = _lseek(fd, 0, SEEK_END);
#else
	off_t size = lseek(fd, 0, SEEK_END);
#endif

	MHD_Response *response = MHD_create_response_from_fd(size, fd);

	return handlesys->CreateHandle(handleTypeResponse, response, NULL, myself->GetIdentity(), NULL);
}

cell_t WebConnection_QueueResponse(IPluginContext *context, const cell_t *params)
{
	HandleError error;

	HandleSecurity security;
	security.pOwner = context->GetIdentity();
	security.pIdentity = myself->GetIdentity();

	MHD_Connection *connection;
	error = handlesys->ReadHandle(params[1], handleTypeConnection, &security, (void **)&connection);
	if (error != HandleError_None) {
		return context->ThrowNativeError("Invalid connection handle %x (error %d)", params[1], error);
	}

	MHD_Response *response;
	error = handlesys->ReadHandle(params[3], handleTypeResponse, &security, (void **)&response);
	if (error != HandleError_None) {
		return context->ThrowNativeError("Invalid response handle %x (error %d)", params[3], error);
	}

	return MHD_queue_response(connection, params[2], response);;
}

cell_t WebConnection_GetClientAddress(IPluginContext *context, const cell_t *params)
{
	HandleSecurity security;
	security.pOwner = context->GetIdentity();
	security.pIdentity = myself->GetIdentity();

	MHD_Connection *connection;
	HandleError error = handlesys->ReadHandle(params[1], handleTypeConnection, &security, (void **)&connection);
	if (error != HandleError_None) {
		return context->ThrowNativeError("Invalid connection handle %x (error %d)", params[1], error);
	}

	sockaddr_in *address = (sockaddr_in *)MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS)->client_addr;
	char *ip = inet_ntoa(address->sin_addr);
	context->StringToLocal(params[2], params[3], ip);

	return 1;
}

cell_t Web_RegisterRequestHandler(IPluginContext *context, const cell_t *params)
{
	char *id;
	context->LocalToString(params[1], &id);
	if (strlen(id) == 0) {
		return 0;
	}

	char *name;
	context->LocalToString(params[3], &name);

	char *description;
	context->LocalToString(params[4], &description);

	if (defaultHandler) {
		delete defaultHandler;
	}

	defaultHandler = new PluginRequestHandler(context, params[2], name, description);

	return 1;
}

sp_nativeinfo_t natives[] = {
	{"WebResponse.AddHeader", WebResponse_AddHeader},
	{"WebStringResponse.WebStringResponse", WebStringResponse_WebStringResponse},
	{"WebBinaryResponse.WebBinaryResponse", WebBinaryResponse_WebBinaryResponse},
	{"WebFileResponse.WebFileResponse", WebFileResponse_WebFileResponse},
	{"WebConnection.QueueResponse", WebConnection_QueueResponse},
	{"WebConnection.GetClientAddress", WebConnection_GetClientAddress},
	{"Web_RegisterRequestHandler", Web_RegisterRequestHandler},
	{NULL, NULL}
};

int DefaultConnectionHandler(void *cls, MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **con_cls)
{
	if (defaultHandler && defaultHandler->callback->GetFunctionCount() == 0) {
		delete defaultHandler;
		defaultHandler = NULL;
	}

	if (!defaultHandler) {
		return MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, responseNotFound);
	}

	if (!defaultHandler->Execute(connection, method, url)) {
		return MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, responseInternalServerError);
	}

	return MHD_YES;
}

void *LogRequestCallback(void *cls, const char *uri, MHD_Connection *con)
{
	//char *ip = inet_ntoa(((sockaddr_in *)MHD_get_connection_info(con, MHD_CONNECTION_INFO_CLIENT_ADDRESS)->client_addr)->sin_addr);
	//smutils->LogMessage(myself, "Request from %s: %s", ip, uri);
	return NULL;
}

void LogErrorCallback(void *cls, const char *fm, va_list ap)
{
	char buffer[2048];
	smutils->FormatArgs(buffer, sizeof(buffer), fm, ap);
	smutils->LogError(myself, "%s", buffer);
}

void NotifyConnectionCallback(void *cls, MHD_Connection *connection, void **socket_context, MHD_ConnectionNotificationCode toe)
{
	Handle_t *handle = (Handle_t *)socket_context;

	HandleError error;

	HandleSecurity security;
	security.pOwner = NULL;
	security.pIdentity = myself->GetIdentity();

	switch(toe) {
		case MHD_CONNECTION_NOTIFY_STARTED:
		{
			*handle = handlesys->CreateHandle(handleTypeConnection, connection, NULL, myself->GetIdentity(), &error);

			if (*handle == BAD_HANDLE) {
				smutils->LogError(myself, "Error creating handle for connection. (%d)", error);
			}

			break;
		}

		case MHD_CONNECTION_NOTIFY_CLOSED:
		{
			if (*handle == BAD_HANDLE) {
				break;
			}

			error = g_pHandleSys->FreeHandle(*handle, &security);

			if (error != HandleError_None) {
				smutils->LogError(myself, "Error freeing handle for connection. (%x, %d)", *handle, error);
			}

			*handle = BAD_HANDLE;

			break;
		}
	}
}

bool Webcon::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	if (!gameconfs->LoadGameConfigFile("webcon.games", &gameConfig, error, maxlength)) {
		return false;
	}

	CDetourManager::Init(smutils->GetScriptingEngine(), gameConfig);

	detourProcessAccept = DETOUR_CREATE_MEMBER(ProcessAccept, "ProcessAccept");
	if (!detourProcessAccept) {
		strncpy(error, "Error setting up ProcessAccept detour", maxlength);
		return false;
	}

	detourRunFrame = DETOUR_CREATE_MEMBER(RunFrame, "RunFrame");
	if (!detourRunFrame) {
		shouldHandleProcessAccept = true;
		smutils->LogError(myself, "WARNING: Error setting up RunFrame detour, all TCP sockets will be hooked.");
	}

	if (!gameConfig->GetMemSig("HandleFailedRconAuth", &CRConServer::HandleFailedRconAuthFunction)) {
		smutils->LogError(myself, "WARNING: HandleFailedRconAuth not found in gamedata, bad clients will not be banned.");
	} else if (!CRConServer::HandleFailedRconAuthFunction) {
		smutils->LogError(myself, "WARNING: Scan for HandleFailedRconAuth failed, bad clients will not be banned.");
	}

	httpDaemon = MHD_start_daemon(MHD_USE_DEBUG | MHD_USE_NO_LISTEN_SOCKET, 0, NULL, NULL, &DefaultConnectionHandler, NULL, MHD_OPTION_URI_LOG_CALLBACK, LogRequestCallback, NULL, MHD_OPTION_EXTERNAL_LOGGER, LogErrorCallback, NULL, MHD_OPTION_NOTIFY_CONNECTION, NotifyConnectionCallback, NULL, MHD_OPTION_END);
	if (!httpDaemon) {
		strncpy(error, "Failed to start HTTP server", maxlength);
		return false;
	}

	const char *contentNotFound = "<!DOCTYPE html>\n<html><body><h1>404 Not Found</h1></body></html>";
	responseNotFound = MHD_create_response_from_buffer(strlen(contentNotFound), (void *)contentNotFound, MHD_RESPMEM_PERSISTENT);

	const char *contentInternalServerError = "<!DOCTYPE html>\n<html><body><h1>500 Internal Server Error</h1></body></html>";
	responseInternalServerError = MHD_create_response_from_buffer(strlen(contentInternalServerError), (void *)contentInternalServerError, MHD_RESPMEM_PERSISTENT);

	handleTypeResponse = handlesys->CreateType("WebResponse", &handlerResponseType, 0, NULL, NULL, myself->GetIdentity(), NULL);

	HandleAccess connectionAccessRules;
	g_pHandleSys->InitAccessDefaults(NULL, &connectionAccessRules);

	connectionAccessRules.access[HandleAccess_Delete] = HANDLE_RESTRICT_IDENTITY;

	handleTypeConnection = handlesys->CreateType("WebConnection", &handlerConnectionType, 0, NULL, &connectionAccessRules, myself->GetIdentity(), NULL);

	sharesys->AddNatives(myself, natives);

	detourProcessAccept->EnableDetour();
	
	if (detourRunFrame) {
		detourRunFrame->EnableDetour();
	}

	return true;
}

void Webcon::SDK_OnUnload()
{
	if (defaultHandler) {
		delete defaultHandler;
	}

	if (detourRunFrame) {
		detourRunFrame->DisableDetour();
	}
	
	detourProcessAccept->DisableDetour();

	handlesys->RemoveType(handleTypeConnection, myself->GetIdentity());

	handlesys->RemoveType(handleTypeResponse, myself->GetIdentity());

	MHD_destroy_response(responseNotFound);

	MHD_stop_daemon(httpDaemon);

	gameconfs->CloseGameConfigFile(gameConfig);
}
