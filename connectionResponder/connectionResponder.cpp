/******************************************************************************
Connection Responder is a one shot proxy server

This program responds to a connection by sending the input file and then outputting what ever
it gets for the next connection or the response on the same connection to standard out.
******************************************************************************/
#define _WSPIAPI_H_
#include <stdio.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <process.h>
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"
#include "Tchar.h"
#include "Winsock2.h"
#include "Ws2tcpip.h"

//# include "Mswsock.h"

//Default values
const TCHAR g_defaultResponseFile[] = _T("Response.txt");
const size_t g_defaultBufferSize = 4096;
const TCHAR g_defaultListeningPort[] = _T("8080");

//Global varibles
HANDLE g_hHaveDataEvent = 0;
HANDLE g_hDoneWritingEvent = 0;
volatile unsigned int g_iShuttingDown = 0;
HANDLE g_hMainThread = 0;
HANDLE g_hOtherThread = 0;


//Various exception classes to represent all of the error conditions

class CBaseException
{
public:
    const char* message;
    const long errorCode;
    CBaseException(const char* Message, const long ErrorCode)
        : message(Message), errorCode(ErrorCode)
    { }
};

class CCommandLineException : public CBaseException
{
public:
    CCommandLineException(const char* Message, const long ErrorCode)
        : CBaseException(Message, ErrorCode)
    { }
};

class COtherException : public CBaseException
{
public:
    COtherException(const char* Message, const long ErrorCode)
        : CBaseException(Message, ErrorCode)
    { }
};

class CSocketException : public CBaseException
{
public:
    CSocketException(const char* Message, const long ErrorCode)
        : CBaseException(Message, ErrorCode)
    { }
};

class CResponseFileException : public CBaseException
{
public:
    CResponseFileException(const char* Message, const long ErrorCode)
        : CBaseException(Message, ErrorCode)
    { }
};

//Initalizes winsock and cleans up with done
class CWinSockManager
{
    public:
    CWinSockManager()
    {
        WSADATA wsaData;
        int iResult = WSAStartup( MAKEWORD(2,2), &wsaData );
        if ( iResult != NO_ERROR )
        {
            throw COtherException("Error while starting networking", WSAGetLastError()); 
        }
    }

    ~CWinSockManager()
    {
        if(WSACleanup() != 0)
        {
            throw COtherException("Error while closing out networking", WSAGetLastError());
        }
    }
};

//Wrapper around malloc/free
class CBuffer
{
public:
    char* buffer;
    CBuffer(size_t Size)
    {
        buffer = (char*) malloc(Size);
        if(NULL == buffer)
        {
            throw COtherException("Error while allacating memory", GetLastError());
        }
    }

    ~CBuffer()
    {
        free(buffer);
    }
};

//Template to call a cleanup routine on exiting current scope
//usage: If function to call is
//         int func(float) 
//       then creation would look like
//         CAutoCloser<int, float, func>(float)
template<class returnType, class argumentType, returnType (__stdcall* closingFunction)(argumentType)>
class CAutoCloser
{
private:
    bool shouldCallClosingFunction;
public:
    argumentType argument;
    CAutoCloser() //seperate ctor for blank condition because arg=0 is valid
        : argument(0), shouldCallClosingFunction(false)
    { }
    CAutoCloser(argumentType arg)
        : argument(arg)
    {
		shouldCallClosingFunction = (arg == 0) ? false : true;
		printf("constructing %p\n", (void*)arg);
    }
    CAutoCloser(CAutoCloser& other)
        : argument(other.argument), shouldCallClosingFunction(other.shouldCallClosingFunction)
    {
        other.shouldCallClosingFunction = false;
		printf("Copy constructor called\n");
    }
	CAutoCloser& operator=(CAutoCloser& other)
	{
		printf("operator= called: %p to %p\n", argument, other.argument);
		argument = other.argument;
		shouldCallClosingFunction = other.shouldCallClosingFunction;
		other.shouldCallClosingFunction = false;
		return *this;
	}
    ~CAutoCloser()
    {
        if(shouldCallClosingFunction)
        {
			printf("calling closing function: %p\n", (void*)argument);
			closingFunction(argument);
        }
    }
};

typedef CAutoCloser<int, SOCKET, closesocket> CSocketHandler;
typedef CAutoCloser<BOOL, HANDLE, CloseHandle> CHandleHandler;
typedef CAutoCloser<void, addrinfo*, freeaddrinfo> CAddrInfoHandler;
typedef CAutoCloser<BOOL, WSAEVENT, WSACloseEvent> CWSAEventHandler;


//Gets the address of the local host in either ip4 or ip6 format
CAddrInfoHandler GetLocalAddressInfo(const TCHAR* listeningPort)
{
    addrinfo hints;
    addrinfo* hostInfo;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;
    //Try and get any localhost address
    //if(0 != getaddrinfo(NULL, listeningPort, &hints, &hostInfo))
    if(0 != getaddrinfo(NULL, "8080", &hints, &hostInfo))
    {
        throw CSocketException("Could not start server", WSAGetLastError());
    }
    return CAddrInfoHandler(hostInfo);
}

//Creates the server and begins listening for connections
CSocketHandler CreateServerSocket(const TCHAR* listeningPort)
{
    CAddrInfoHandler addressInfo(GetLocalAddressInfo(listeningPort));

    SOCKET listeningSocket = socket(PF_UNSPEC, SOCK_STREAM, IPPROTO_TCP);
    if(INVALID_SOCKET == listeningSocket)
    {
        throw CSocketException("Error creating socket", WSAGetLastError());
    }

    addrinfo* current = addressInfo.argument;
    for(; current != NULL; current = current->ai_next)
    {
        if((current->ai_family == PF_INET) || (current->ai_family == PF_INET6))
        {
			break;
        }
    }
    //Last param of bind is an int instead of a size_t.
    if(SOCKET_ERROR == bind(listeningSocket, current->ai_addr, (int)current->ai_addrlen))
    {
        throw CSocketException("Error binding to socket", WSAGetLastError());
    }
    if(SOCKET_ERROR == listen(listeningSocket, 2))
    {
        throw CSocketException("Error listening to socket", WSAGetLastError());
    }

    return CSocketHandler(listeningSocket);
}

//Gets the first connection from the server socket.
CSocketHandler GetFirstConnection(CSocketHandler& ListeningSocket)
{
    SOCKET newConnectionSocket = accept(ListeningSocket.argument, NULL, NULL);
    if(INVALID_SOCKET == newConnectionSocket)
    {
        throw CSocketException("Error accepting socket", WSAGetLastError());
    }
    return CSocketHandler(newConnectionSocket);
}

void SendResponseFile(CSocketHandler& OutputSocket, CHandleHandler& ResponseFile)
{
    printf("doing SendResponseFile\n");
	CBuffer buffer(g_defaultBufferSize);
    DWORD readBytes;
    do
    {
        if(0 == ReadFile(ResponseFile.argument, buffer.buffer, g_defaultBufferSize, &readBytes, NULL))
        {
            throw CResponseFileException("Error while reading the response file", GetLastError());
        }
        if(readBytes > 0)
        {
            if(SOCKET_ERROR == send(OutputSocket.argument, buffer.buffer, (int)readBytes, 0))
            {
                throw CSocketException("Error while sending the response file", WSAGetLastError());
            }
        }
    } while(readBytes > 0);
}

//Get the handle that all output is directed (Standard out)
HANDLE GetOutputHandle()
{
    HANDLE outputFileHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if(outputFileHandle == INVALID_HANDLE_VALUE)
    {
        throw CResponseFileException("Can't open output", GetLastError());
    }
    return outputFileHandle;
}

void ReadAndOutputData(SOCKET* InputSocket)
{
    CBuffer buffer(g_defaultBufferSize);
    HANDLE outputFileHandle = GetOutputHandle();
    unsigned long availableBytes = 0;
    DWORD dwResult = 0;
    
	printf("Doing ReadAndOutputData\n");
    do
    {
        int iResult = recv(*InputSocket, buffer.buffer, g_defaultBufferSize, 0);
        if(SOCKET_ERROR == iResult)
        {
            throw CSocketException("Error while reading from socket", WSAGetLastError());
        }
        if(0 == WriteFile(outputFileHandle, buffer.buffer, (DWORD) iResult, &dwResult, NULL))
        {
            throw COtherException("Error writing output", GetLastError());
        }
        ioctlsocket(*InputSocket, FIONREAD, &availableBytes);
    } while (availableBytes > 0);
    //Terminate the client message with a new line.
    char endOfLineBuffer[] = "\n";
    if(0 == WriteFile(outputFileHandle, endOfLineBuffer, sizeof(endOfLineBuffer)/sizeof(char), &dwResult, NULL))
    {
        throw COtherException("Error writing output", GetLastError());
    }
}

//taking into account spaces and double quotes
LPTSTR getNextArgument(LPTSTR Input)
{
    if(Input == NULL)
    {
        return NULL;
    }
    bool outsideQuotes = true;
    for(LPTSTR i = Input; *i != '\0'; i +=sizeof(TCHAR))
    {
        switch(*i)
        {
        case '\0':
            return NULL;
        case '"':
            outsideQuotes = !outsideQuotes;
            break;
        case ' ':
            if(outsideQuotes) // is this an odd quoteCount
            {
                return i + sizeof(TCHAR);
            }
            break;
        default:
            break;
        }
    }
    return NULL;
}

//Searchs the command line for port and response file arguments
void GetPortAndResponseFile(const TCHAR* &Port, LPCTSTR &ResponseFileStart, LPTSTR &ResponseFileEnd)
{
    for(LPTSTR current = getNextArgument(GetCommandLine()), next = getNextArgument(current);
        current != NULL;
        current = getNextArgument(next), next = getNextArgument(current))
    {
        if((current[0] != '-') && (current[0] != '/'))// all arguments must start with a - or /
        {
            continue;
        }
        switch(current[1])
        {
        case 'p':
        case 'P':
            if(next != NULL)
            {
				Port = next;
            }
            else
                throw CCommandLineException("Port parameter is incorrect", 0);
            break;
        case 'r':
        case 'R':
        case 'f':
        case 'F':
            if(next != NULL)
            {
                ResponseFileStart = next;
                ResponseFileEnd = getNextArgument(next);
            }
            else
            {
                throw CCommandLineException("Response file parameter is incorrect", 0);
            }
            break;
        case '\0': // end of commandLine
        default:
            throw CCommandLineException("Unknown parameter option", 0);
        }
    }
}

//Searchs the command line for arguments.  Those it does not find it filles in the default
//Returns the response file since it is a required argument.
CHandleHandler ProcessArguments(const TCHAR*& listeningPort)
{
    LPCTSTR responseFileStart = 0;
    LPTSTR responseFileEnd = 0;
    GetPortAndResponseFile(listeningPort, responseFileStart, responseFileEnd);
    if(responseFileStart == 0) // response file not on command line
    {
        responseFileStart = g_defaultResponseFile;
    }
    else if(0 != responseFileEnd) //using command line so need to end string before next parameter
    {
        responseFileEnd[-1] = '\0';
    }
    HANDLE responseFileHandle = CreateFile(responseFileStart, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if(responseFileEnd != 0) //reset changed character
    {
        responseFileEnd[-1] = ' ';
    }
    if(responseFileHandle == INVALID_HANDLE_VALUE)
    {
        throw CCommandLineException("Can not open response file", GetLastError());
    }

    return CHandleHandler(responseFileHandle);
}

WSAEVENT CreateEventFor(SOCKET Socket, long networkEvents)
{
	printf("calling CreateEventFor(%p, %i)\n", Socket, (int)networkEvents);
	WSAEVENT newEvent = WSACreateEvent();
	if(newEvent == NULL)
	{
		throw CSocketException("Could not create event", WSAGetLastError());
	}
	if(SOCKET_ERROR == WSAEventSelect(Socket, newEvent, networkEvents))
	{
		throw CSocketException("Could not setup event", WSAGetLastError());
	}
	return newEvent;
}

void HandleDataEvent(SOCKET* Socket, WSAEVENT DataEvent, int& numberOfOutputs)
{
	++numberOfOutputs;
	ReadAndOutputData(Socket);
	WSAResetEvent(DataEvent);
}

int __cdecl main(int, char** argv)
{
    try
    {
        const TCHAR* listeningPort = g_defaultListeningPort;
        CHandleHandler responseFile(ProcessArguments(listeningPort));
        CWinSockManager WSAManager;

        CSocketHandler listeningSocket(CreateServerSocket(listeningPort));
		const DWORD AcceptEvent = 0, MainDataEvent = 1, OtherDataEvent = 2;
		WSAEVENT events[] = {CreateEventFor(listeningSocket.argument, FD_ACCEPT), 0, 0};
		CSocketHandler dataSockets[] = {0, 0};
		DWORD numberOfEvents = 1;
		int numberOfOutputs = 0;
		while(1)
		{
			DWORD signaledEvent = WSAWaitForMultipleEvents(numberOfEvents, events, FALSE, WSA_INFINITE, FALSE);
			switch(signaledEvent - WSA_WAIT_EVENT_0)
			{
			case AcceptEvent:
				if(numberOfEvents < (sizeof(events)/sizeof(WSAEVENT)))
				{
					dataSockets[numberOfEvents - 1] = GetFirstConnection(listeningSocket);
					events[numberOfEvents] = CreateEventFor(dataSockets[numberOfEvents - 1].argument, FD_READ);
					//Check if data came with the connect
					if(0 == (WSAWaitForMultipleEvents(1, &events[numberOfEvents], FALSE, 1, FALSE) - WSA_WAIT_EVENT_0))
					{
						HandleDataEvent(&dataSockets[numberOfEvents - 1].argument, events[numberOfEvents], numberOfOutputs);
					}
					//send file if first connection
					if(dataSockets[1].argument == 0)
					{
						SendResponseFile(dataSockets[numberOfEvents - 1], responseFile);
					}
					++numberOfEvents;
					WSAResetEvent(events[AcceptEvent]);
				}
				break;
			case MainDataEvent:
				printf("doing MainDataEvent\n");
				HandleDataEvent(&dataSockets[0].argument, events[MainDataEvent], numberOfOutputs);
				break;
			case OtherDataEvent:
				printf("doing OtherDataEvent\n");
				HandleDataEvent(&dataSockets[1].argument, events[OtherDataEvent], numberOfOutputs);
				break;
			case WSA_WAIT_FAILED:
			default:
				throw CSocketException("Error while waiting for network events", WSAGetLastError());
			}
			if(numberOfOutputs >= 2)
			{
				break;
			}
		}
	}
    catch(CCommandLineException&)
    {
        printf("%s [-p Port] [-r Response File]\n"
               "Default port: %s\nDefualt response file: %s\n"
               "\n", argv[0], g_defaultListeningPort, g_defaultResponseFile);
    }
    catch(COtherException& e)
    {
        fprintf(stderr, "%s: %ld\n", e.message, e.errorCode);
        return -1;
    }
    catch(CResponseFileException& e)
    {
        fprintf(stderr, "%s: %ld\n", e.message, e.errorCode);
        return -1;
    }
    catch(CSocketException& e)
    {
        fprintf(stderr, "%s: %ld\n", e.message, e.errorCode);
        return -1;
    }
    return 0;
}
