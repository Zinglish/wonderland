#include "ipc_server_man.h"
#include "ipc_comm.h"
#include "../globals.h"

#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>

std::vector<IPCComm*>      IPCServer::clientComms;
std::vector<IPCCoD4Event*> IPCServer::broadcastEvents;
std::mutex                 IPCServer::bcastEventStackLock;

IPCServer::IPCServer(std::string path) {
	// Initialize the client holder
	this->clientComms.reserve(5);

	// Allocate some memory for the broadcastEvents stack
	IPCServer::broadcastEvents.reserve(20);
	
	this->clientCommPrefix = "rabbithole-";
	this->clientCommPath   = "/tmp/";
	
	// Create the management thread
	pthread_create(&this->listener, NULL, &IPCServer::ThreadedCommAllocator, this);
}

IPCServer::IPCServer(const IPCServer& orig) {}
IPCServer::~IPCServer() {}

/*===============================================================*\
 * THREADS
\*===============================================================*/

/**
 * Manages incoming link requests to the server. If the client is allowed to then
 * the thread will construct a new link + thread to communicate fully with Alice.
 */
void* IPCServer::ThreadedCommAllocator(void* serverPtr) {
	IPCServer* server = (IPCServer*) serverPtr;
	
	int localSock;
	struct sockaddr_un local;

	// Create the socket
	if((localSock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		printf("Socket creation failed, error: %i\n", errno);
		exit(1);
	}
	
	// Prepare and bind the socket
	local.sun_family = AF_UNIX;
	strcpy(local.sun_path, "/tmp/wonderland");
	unlink(local.sun_path); // Delete the original socket
	unsigned int len = strlen(local.sun_path) + sizeof(local.sun_family);
	
	if(bind(localSock, (struct sockaddr *)&local, len) == -1) {
		printf("Binding failed, error: %i\n", errno);
		exit(1);
	}

	// Start listening
	if (listen(localSock, 5) == -1) {
		printf("Listen failed, error: %i\n", errno);
		exit(1);
	}
        
	printf("IPC (v%i) server started\n", IPC_VER);
	
	// Accept connections indefinitely
	while(true) {
		printf("Awaiting new connection\n");
		
		struct sockaddr_un remote;
		int clientSock;
		unsigned int clientSockSize = sizeof(remote);
		
		// Hang until a connection is incoming
		if((clientSock = accept(localSock, (struct sockaddr *)&remote, &clientSockSize)) == -1) {
			printf("Accept failed, error: %i\n", errno);
			exit(1);
		}
		
		printf("Connection established\n");
		
		// Once connected we need to get data from the client
		char rxRaw[8]; // 8bytes (1st int is version, 2nd int is size of payload)
		std::string rxString = ""; // Used for debugging
		int rxStatus = 0;
		
		char* payload = NULL;
		
		while(true) {
			rxStatus = recv(clientSock, &rxRaw, 8, 0);
			
			// An error occurred -OR- EOS
			if(rxStatus <= 0)
				break;

			// Get the version and payload length
			u_int32_t version    = ntohl(*(u_int32_t*) &rxRaw[0]);
			u_int32_t payloadLen = ntohl(*(u_int32_t*) &rxRaw[4]);

			// Double check protocol version
			if(version != (u_int32_t) IPC_VER)
			{
				printf("IPC protocol version mismatch (RX: %i | IPC_VER: %i)\n", version, IPC_VER);
				break;
			}

			// Get the full payload
			payload = server->RecvChunk(clientSock, payloadLen);
			
			// General debugging
			printf("Version: %i\n", version);
			printf("Payload length: %i\n", payloadLen);
			printf("Payload: %s\n", payload);

			// Handle the response back to the client
			server->ResponseHandler(clientSock, payload);
			
			delete payload;
		}

		printf("Client disconnected\n");
	}
}



/*===============================================================*\
 * FUNCTIONS
\*===============================================================*/

char* IPCServer::RecvChunk(int socket, u_int32_t chunkSize)
{
	// Make space for the payload, setting the last byte to 0x00 as if
	// it was a C style string
	char* payload = new char[chunkSize + 1];
	memset(&payload[chunkSize], 0x00, 1);
	
	unsigned int curPos     = 0;
	unsigned int currLen    = 0; // Amount of chars written to payload
	unsigned int bufferSize = 8; // Recv 8 bytes every read
	
	while(curPos < chunkSize)
	{
		currLen = chunkSize - curPos;

		// Set the recv size under certain circumstances
		if(currLen < bufferSize)
			bufferSize = currLen;
		if(chunkSize < bufferSize)
			bufferSize = chunkSize;

		recv(socket, &payload[curPos], bufferSize, 0);
		
		curPos += bufferSize;
	}
	
	return payload;
}

void IPCServer::ResponseHandler(int socket, char* pkt)
{
	char* payload = NULL; // Modify payload to send it back to the client

	// A request to go deeper into the rabbit hole
	if(strncmp("RABBITHOLE", pkt, 10) == 0)
	{
		// Prepare the full path to the rabbit hole
		unsigned int commId = this->CreateNewComm();
		
		std::string fullCommPath = clientComms.at(commId)->GetPath();
		payload = new char[fullCommPath.length() + 1];
		
		fullCommPath.copy(payload, fullCommPath.length(), 0);
		
		payload[fullCommPath.length()] = '\0';
	}
	// Version request
	else if(strncmp("VERSION", pkt, 7) == 0)
	{
		unsigned int s = strlen(WONDERLAND_VER);
		payload = new char[s + 1];
		memcpy(payload, WONDERLAND_VER, s + 1);
	}
	
	// If the response handler matched any commands
	if(payload != NULL)
	{
		unsigned int payloadSize = strlen(payload);
		char* packet = new char[4 + payloadSize + 1];
		
		// Prepare the packet
		u_int32_t s = htonl(payloadSize);
		memcpy(packet, &s, 4);
		memcpy(packet + 4, payload, payloadSize);
		packet[4 + payloadSize] = '\0';
		
		send(socket, packet, 4 + payloadSize, 0);
		
		delete packet;
		delete payload;
	}
	
	return;
}

unsigned int IPCServer::CreateNewComm()
{
	// Prepare args for creating a new comm
	unsigned int commId = this->clientComms.size();
	
	// Create a new comm and push it into the array
	this->clientComms.push_back(new IPCComm(commId, this->clientCommPath, this->clientCommPrefix));
	
	return commId;
}

void IPCServer::SetEventForBroadcast(IPCCoD4Event* event)
{
	// Add the event to the broadcast events vector
	IPCServer::bcastEventStackLock.lock();
	
	bool found = false;
	unsigned int s = IPCServer::broadcastEvents.size();
	for(unsigned int i = 0; i < s; i++)
	{
		if(IPCServer::broadcastEvents[i] == NULL)
		{
			IPCServer::broadcastEvents[i] = event;
			found = true;

			break;
		}
	}

	// If no free spot was found in the list then just push into the stack
	if(!found)
		IPCServer::broadcastEvents.push_back(event);
	
	IPCServer::bcastEventStackLock.unlock();

	// Signal all comm channels that an event has triggered
	IPCComm* rabbitHole = NULL;
	s = IPCServer::clientComms.size();
	for(unsigned int i = 0; i < s; i++)
	{
		rabbitHole = IPCServer::clientComms[i];
		rabbitHole->SignalSend();
	}
	rabbitHole = NULL;
}

void IPCServer::DestroyEvent(IPCCoD4Event* event)
{
	//IPCServer::bcastEventStackLock.lock();

	unsigned int s = IPCServer::broadcastEvents.size();
	for(unsigned int i = 0; i < s; i++)
	{
		if(event == IPCServer::broadcastEvents[i])
		{
			// Free the memory first
			delete IPCServer::broadcastEvents[i];

			IPCServer::broadcastEvents[i] = NULL;
		}	
	}

	//IPCServer::bcastEventStackLock.unlock();
}