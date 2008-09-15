// all server side masterserver and pinging functionality

#include "pch.h"
#include "cube.h"

#ifdef STANDALONE
bool resolverwait(const char *name, ENetAddress *address)
{
    return enet_address_set_host(address, name) >= 0;
}

int connectwithtimeout(ENetSocket sock, const char *hostname, ENetAddress &remoteaddress)
{
    int result = enet_socket_connect(sock, &remoteaddress);
    if(result<0) enet_socket_destroy(sock);
    return result;
}
#endif

char *httpgetrequest(const char *hostname, const char *uripart, const char *ref, const char *agent)
{
    char *s = newstring(_MAXDEFSTR);
    s_sprintf(s)("GET %s HTTP/1.0\nHost: %s\nReferer: %s\nUser-Agent: %s\n\n", uripart, hostname, ref, agent);
    return s;
}

ENetSocket httpgetsend(ENetAddress &remoteaddress, const char *hostname, char *request, ENetAddress *localaddress = NULL, ENetSocketType sockettype = ENET_SOCKET_TYPE_STREAM)
{
    if(remoteaddress.host==ENET_HOST_ANY)
    {
#ifdef STANDALONE
        printf("looking up %s...\n", hostname);
#endif
        if(!resolverwait(hostname, &remoteaddress)) return ENET_SOCKET_NULL;
    }
    ENetSocket sock = enet_socket_create(sockettype, localaddress);
    if(sock==ENET_SOCKET_NULL || (sockettype==ENET_SOCKET_TYPE_STREAM && connectwithtimeout(sock, hostname, remoteaddress)<0))
    {
#ifdef STANDALONE
        printf(sock==ENET_SOCKET_NULL ? "could not open socket\n" : "could not connect\n");
#endif
        return ENET_SOCKET_NULL;
    }
    enet_socket_set_option(sock, ENET_SOCKOPT_BROADCAST, 1);
    ENetBuffer buf;
    buf.data = request;
    buf.dataLength = strlen((char *)buf.data);
#ifdef STANDALONE
    printf("sending request to %s...\n", hostname);
#endif
    enet_socket_send(sock, sockettype==ENET_SOCKET_TYPE_STREAM ? NULL : &remoteaddress, &buf, 1);
    return sock;
}

bool httpgetreceive(ENetSocket sock, ENetBuffer &buf, int timeout = 0)
{
    if(sock==ENET_SOCKET_NULL) return false;
    enet_uint32 events = ENET_SOCKET_WAIT_RECEIVE;
    if(enet_socket_wait(sock, &events, timeout) >= 0 && events)
    {
        int len = enet_socket_receive(sock, NULL, &buf, 1);
        if(len<=0)
        {
            enet_socket_destroy(sock);
            return false;
        }
        buf.data = ((char *)buf.data)+len;
        ((char*)buf.data)[0] = 0;
        buf.dataLength -= len;
    }
    return true;
}

uchar *stripheader(uchar *b)
{
    char *s = strstr((char *)b, "\n\r\n");
    if(!s) s = strstr((char *)b, "\n\n");
    return s ? (uchar *)s : b;
}

ENetSocket mssock = ENET_SOCKET_NULL;
ENetAddress masterserver = { ENET_HOST_ANY, 80 };
ENetAddress msaddress = { ENET_HOST_ANY, ENET_PORT_ANY };
ENetAddress machinemasterserver = { ENET_HOST_ANY, CUBE_SERVINFO_PORT_LAN };
int lastupdatemaster = 0;
string masterbase;
string masterpath;
uchar masterrep[MAXTRANS];
ENetBuffer masterb;

void updatemasterserver(int millis, ENetAddress &localaddr)
{
    if(millis/(60*60*1000)!=lastupdatemaster)       // send alive signal to masterserver every hour of uptime
    {
        char *request;
		s_sprintfd(path)("%sregister.do?action=add&port=%d", masterpath, localaddr.port);
        s_sprintfd(agent)("AssaultCube Server %d", AC_VERSION);
        s_sprintfd(ref)("assaultcubeserver");

        // register on public masterserver
        request = httpgetrequest(masterbase, path, ref, agent);
		mssock = httpgetsend(masterserver, masterbase, request, &msaddress);
        delete[] request;
		masterrep[0] = 0;
		masterb.data = masterrep;
		masterb.dataLength = MAXTRANS-1;
        
        // register on local machine masterserver
        localmasterserver.create(machinemasterserver); // try creating an instance, maybe we are the first or the only local server
        string localhost;
        if(machinemasterserver.host==ENET_HOST_ANY)
        {
            s_strcpy(localhost, "localhost");
        }
        else enet_address_get_host_ip(&machinemasterserver, localhost, _MAXDEFSTR);
        request = httpgetrequest(localhost, path, ref, agent);
        ENetSocket lansock = httpgetsend(machinemasterserver, localhost, request, &msaddress, ENET_SOCKET_TYPE_DATAGRAM);
        delete[] request;
        enet_socket_destroy(lansock);

        lastupdatemaster = millis/(60*60*1000);
    }
}

void checkmasterreply()
{
    if(mssock!=ENET_SOCKET_NULL && !httpgetreceive(mssock, masterb))
    {
        mssock = ENET_SOCKET_NULL;
        printf("masterserver reply: %s\n", stripheader(masterrep));
    }
}

#ifndef STANDALONE

#define RETRIEVELIMIT 20000
#define BROADCASTLIMIT 5000

uchar *retrieveservers(uchar *buf, int buflen, ENetAddress &masterserver, char *masterpath)
{
    bool lansearch = (masterserver.host==ENET_HOST_BROADCAST);
    buf[0] = '\0';

    s_sprintfd(path)("%sretrieve.do?item=list", masterpath);
    s_sprintfd(agent)("AssaultCube Client %d", AC_VERSION);
    ENetAddress address = masterserver;
    ENetSocket sock = ENET_SOCKET_NULL;

    if(lansearch)
    {
        char *request = httpgetrequest("", path, "assaultcubeclient", agent);
        sock = httpgetsend(masterserver, "localhost", request, NULL, ENET_SOCKET_TYPE_DATAGRAM);
    }
    else
    {
        char *request = httpgetrequest(masterbase, path, "assaultcubeclient", agent);
        sock = httpgetsend(address, masterbase, request);
    }

    if(sock==ENET_SOCKET_NULL) return buf;
    // only cache this if connection succeeds
    masterserver = address;

    ENetBuffer eb;
    eb.data = buf;
    eb.dataLength = buflen-1;
    int starttime = SDL_GetTicks(), timeout = 0;

    if(lansearch) // broadcast lan
    {
        s_sprintfd(text)("searching servers in your LAN... (esc to abort)");

        while(timeout < BROADCASTLIMIT)
        {
            timeout = SDL_GetTicks() - starttime;
            show_out_of_renderloop_progress(min(float(timeout)/RETRIEVELIMIT, 1.0f), text);
            SDL_Event event;
            while(SDL_PollEvent(&event))
            {
                if(event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) timeout = RETRIEVELIMIT + 1;
            }

            // collect responses
            enet_uint32 events = ENET_SOCKET_WAIT_RECEIVE;
            uchar lanbuf[32000] = { 0 };
            ENetBuffer laneb;
            laneb.data = lanbuf;
            laneb.dataLength = sizeof(lanbuf);

            if(enet_socket_wait(sock, &events, 250) >= 0 && events)
            {
                // strip response
                int len = enet_socket_receive(sock, NULL, &laneb, 1);
                if(len<=0) continue;
                uchar *bodybegin = stripheader(lanbuf);
                size_t headersize = bodybegin-lanbuf;
                
                // collect responses into a single buffer
                memcpy(eb.data, lanbuf+headersize, len-headersize);
                eb.data = ((char *)eb.data)+len-headersize;
                ((char*)eb.data)[0] = 0;
                eb.dataLength -= len-headersize;
            }
        }

        enet_socket_destroy(sock);
        return buf;
    }
    else // contact public masterserver
    {
        s_sprintfd(text)("retrieving servers from %s... (esc to abort)", masterbase);
        show_out_of_renderloop_progress(0, text);

        // reconstruct multipacket reponse
        while(httpgetreceive(sock, eb, 250))
        {
            timeout = SDL_GetTicks() - starttime;
            show_out_of_renderloop_progress(min(float(timeout)/RETRIEVELIMIT, 1.0f), text);
            SDL_Event event;
            while(SDL_PollEvent(&event))
            {
                if(event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) timeout = RETRIEVELIMIT + 1;
            }
            if(timeout > RETRIEVELIMIT)
            {
                buf[0] = '\0';
                enet_socket_destroy(sock);
                return buf;
            }
        }

        enet_socket_destroy(sock);
        return stripheader(buf);
    }
}
#endif

ENetSocket pongsock = ENET_SOCKET_NULL;
string serverdesc;

void serverms(int mode, int numplayers, int minremain, char *smapname, int millis, ENetAddress &localaddr)
{
    checkmasterreply();
    updatemasterserver(millis, localaddr);

	// reply all server info requests
	ENetBuffer buf;
    ENetAddress addr;
    uchar data[MAXTRANS];
    buf.data = data;
    int len;
    enet_uint32 events = ENET_SOCKET_WAIT_RECEIVE;
    while(enet_socket_wait(pongsock, &events, 0) >= 0 && events)
    {
        buf.dataLength = sizeof(data);
        len = enet_socket_receive(pongsock, &addr, &buf, 1);
        if(len < 0) return;

        // ping & pong buf
        ucharbuf pi(data, sizeof(data));
        ucharbuf po(&data[len], sizeof(data)-len);

        if(getint(pi) != 0) // std pong
        {
            putint(po, PROTOCOL_VERSION);
            putint(po, mode);
            putint(po, numplayers);
            putint(po, minremain);
            sendstring(smapname, po);
            sendstring(serverdesc, po);
            putint(po, maxclients);
        }
        else // ext pong - additional server infos
        {
            int extcmd = getint(pi);
            putint(po, EXT_ACK);
            putint(po, EXT_VERSION);

            switch(extcmd)
            {
                case EXT_UPTIME:        // uptime in seconds
                {
                    putint(po, millis/1000);
                    break;
                }

                case EXT_PLAYERSTATS:   // playerstats
                {
                    int cn = getint(pi);     // get requested player, -1 for all
                    if(!valid_client(cn) && cn != -1)
                    {
                        putint(po, EXT_ERROR);
                        break;
                    }
                    putint(po, EXT_ERROR_NONE);              // add no error flag

                    int bpos = po.length();                  // remember buffer position
                    putint(po, EXT_PLAYERSTATS_RESP_IDS);    // send player ids following
                    extinfo_cnbuf(po, cn);
                    buf.dataLength = len + po.length();
                    enet_socket_send(pongsock, &addr, &buf, 1); // send all available player ids
                    po.len = bpos;

                    extinfo_statsbuf(po, cn, bpos, pongsock, addr, buf, len);
                    return;
                }

                case EXT_TEAMSCORE:
                    extinfo_teamscorebuf(po);
                    break;

                default:
                    putint(po,EXT_ERROR);
                    break;
            }
        }

        buf.dataLength = len + po.length();
        enet_socket_send(pongsock, &addr, &buf, 1);
    }
}

void servermsinit(const char *master, const char *ip, int infoport, const char *sdesc, bool listen)
{
	const char *mid = strstr(master, "/");
    if(mid) s_strncpy(masterbase, master, mid-master+1);
    else s_strcpy(masterbase, (mid = master));
    s_strcpy(masterpath, mid);
    s_strcpy(serverdesc, sdesc);

	if(listen)
	{
        ENetAddress address = { ENET_HOST_ANY, infoport };
        if(*ip && enet_address_set_host(&address, ip)<0) printf("WARNING: server ip not resolved");
        if(*ip && enet_address_set_host(&msaddress, ip)<0) printf("WARNING: server ip not resolved");
        if(*ip && enet_address_set_host(&machinemasterserver, ip)<0) printf("WARNING: server ip not resolved");
        pongsock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM, &address);
        if(pongsock == ENET_SOCKET_NULL) fatal("could not create server info socket\n");
        else enet_socket_set_option(pongsock, ENET_SOCKOPT_NONBLOCK, 1);
	}
}

void servermsdesc(const char *sdesc)
{
    s_strcpy(serverdesc, sdesc);
}
