/*
 *  DeployHub is an Agile Application Release Automation Solution
 *  Copyright (C) 2017 Catalyst Systems Corporation DBA OpenMake Software
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as
 *  published by the Free Software Foundation, either version 3 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef WIN32
#include "windows.h"
#include <io.h>
#include <fcntl.h>
#define HTTP_EXPORT __declspec(dllexport)
#else
#define HTTP_EXPORT
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#define _O_WRONLY O_WRONLY 
#define _O_BINARY 0
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#endif /*WIN32*/

#include <stdio.h>
#include <sys/stat.h>
#ifdef WIN32
#include <direct.h>
#include <sys/utime.h>
#else
#include <sys/time.h>
#endif /*WIN32*/
#include <errno.h>

#include "http.h"

#include "cmdline.h"
#include "exceptions.h"
#include "expr.h"
#include "charptr.h"
#include "node.h"
#include "context.h"
#include "dm.h"
#include "properties.h"
#include "logprocess.h"
#include "execute.h"
#include "dropzone.h"
#include "httppattern.h"


#define HTTP_PLUGIN_NAME    "http"
#define HTTP_REPO_NAME      "http"

#define HTTP_PLUGIN_VERSION "1.00"

#define HTTP_DEFAULT_PORT 80

//
// Same code as in smtp.cpp but replicated here because this is in its own library
//
char *base64encode(unsigned char *data, unsigned long datalen)
{
	static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int len = datalen/3 * 4;
	if(datalen % 3 > 0) len += 4;
	len += (len/76)*2;
	char *output = (char*) malloc(len+1);
	if (output) {

		// 11111100 = fc / 03; 11110000 = f0 / 0f; 11000000 = c0 / 3f
		int m = 0;
		unsigned m1, m2, m3, m4;
		int maxn = (datalen/3) * 3;
		for(int n = 0; n < maxn; n += 3)
		{
			m1 = (data[n] & 0xfc) >> 2; 
			m2 = ((data[n] & 0x03) << 4) + ((data[n+1] & 0xf0) >> 4);
			m3 = ((data[n+1] & 0x0f) << 2) + ((data[n+2] & 0xc0) >> 6);
			m4 = data[n+2] & 0x3f;
			output[m++] = b64[m1];
			output[m++] = b64[m2];
			output[m++] = b64[m3];
			output[m++] = b64[m4];

			if((n+3) % 57 == 0) {
				output[m++] = 0x0d;
				output[m++] = 0x0a;
			}
		}

		switch(datalen % 3)
		{
		case 0:
			break;
		case 1:
			m1 = (data[maxn] & 0xfc) >> 2;
			m2 = (data[maxn] & 0x03) << 4;
			output[m++] = b64[m1];
			output[m++] = b64[m2];
			output[m++] = '=';
			output[m++] = '=';
			break;
		case 2:
			m1 = (data[maxn] & 0xfc) >> 2;
			m2 = ((data[maxn] & 0x03) << 4) + ((data[maxn+1] & 0xf0) >> 4);
			m3 = ((data[maxn+1] & 0x0f) << 2);
			output[m++] = b64[m1];
			output[m++] = b64[m2];
			output[m++] = b64[m3];
			output[m++] = '=';
			break;
		}

		output[m] = '\0';
	}
	return output;
}

// Simple URL encoder. Space = %20. Leaves / alone so we can encode a complete path
char to_hex(char code)
{
	static char hex[] = "0123456789abcdef";
	return hex[code & 15];
}

char *url_encode(char *str)
{
	char *pstr = str;
	char *buf = (char *)malloc(strlen(str) * 3 + 1);
	char *pbuf = buf;
	while (*pstr) {
		if (isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' || *pstr == '~') 
			*pbuf++ = *pstr;
		else if (*pstr == '/')
			*pbuf++ = '/';
		else {
			*pbuf++ = '%';
			*pbuf++ = to_hex(*pstr >> 4);
			*pbuf++ = to_hex(*pstr & 15);
		}
		pstr++;
	}
	*pbuf = '\0';
	return buf;
}


///////////////////////////////////////////////////////////////////////////////

void SendDataToSocket(int sock, const char *data, long len)
{
	long bytesRemaining = len;

	while(bytesRemaining > 0)
	{
        int bytesSent = send(sock, data, bytesRemaining, 0);
        if(bytesSent == 0)
        {
			return;
        }
        else if(bytesSent == -1)
        {
            perror("Failed to send");
			return;
        }

		data += bytesSent;
		bytesRemaining -= bytesSent;
	}
}


bool ReadDataFromSocket(int sock, char *dataPtr, long length)
{
	// Reads "length" bytes of data from the socket.
	// If less than "Length" bytes are read then loop.
	//
	long		BytesRead=0;
	char 		*xPtr;
	long		BytesToRead;
	bool		res=true;

	xPtr=(char *)dataPtr;
	BytesToRead=length;
	bool BytesRemaining=true;
	while (BytesRemaining)
	{
		BytesRead=recv(sock,xPtr,BytesToRead,0);
		if (BytesRead == -1)
		{
			// ShowErrorMessage("Failed to read from socket");
			BytesRemaining=false;
			res=false;
			break;
		}
		if (BytesRead == 0)
		{
			// Server has closed connection.
			// ShowErrorMessage("Server has closed connection");
			BytesRemaining=false;
			res=false;
			break;
		}
		if (BytesRead<BytesToRead)
		{
			BytesToRead=BytesToRead-BytesRead;
			xPtr=xPtr+BytesRead;
		}
		else
		{
			BytesRemaining=false;
		}
	}
	return res;
}

char *stristr (char *ch1, char *ch2)
{
	char  *chN1, *chN2;
	char  *chNdx;
	char  *chRet        = NULL;

	chN1 = strdup (ch1);
	chN2 = strdup (ch2);
	if (chN1 && chN2) {
		chNdx = chN1;
		while (*chNdx) {
			*chNdx = (char) tolower (*chNdx);
			chNdx ++;
		}
		chNdx = chN2;
		while (*chNdx) {
			*chNdx = (char) tolower (*chNdx);
			chNdx ++;
		}
		chNdx = strstr (chN1, chN2);
		if (chNdx) chRet = ch1 + (chNdx - chN1);
	}
	free (chN1);
	free (chN2);
	return chRet;
}



char *ReadToEndOfStream(int sock,int *retlen, bool CheckForHTML)
{
	// Reads bytes of data from a socket until the end of the stream is reached.
	//
	int			currSize = 0;
	char		*ret = NULL;
	char		xBuf[8192];

	while(true) {
		long bytesRead = recv(sock, xBuf, sizeof(xBuf), 0);
		debug1("read %d bytes",bytesRead);
		if(bytesRead == -1) {
			// ShowErrorMessage("Failed to read from socket");
			break;
		}
		if(bytesRead == 0) {
			// Server has closed connection.
			break;
		}

		ret = ret ? (char*) realloc(ret, currSize + bytesRead + 1) : (char*) malloc(bytesRead + 1);

		memcpy(&ret[currSize], xBuf, bytesRead);
		currSize += bytesRead;
		ret[currSize] = '\0';
		if (stristr(ret,"</html>")) break;

	}
	*retlen = currSize;
	return ret;
}


char *ReadLineFromSocket(int sock)
{
	char Reply[1024];
	int n;

	Reply[0] = '\0';

	// Read up to CR-LF
	char ch;
	int t=0;
	do
	{
		n = recv(sock,&ch,1,0);
		if(n==0)
		{
			// Connection has been closed
			ch='\0';
		}
		else if(n==1)
		{
			if (ch=='\n' || t>=(sizeof(Reply)-1)) ch='\0';
			if (ch!='\r') Reply[t++]=ch;
		}
		else
		{
#ifdef _DEBUG
			// RHT 15/07/2008 - try and figure out why we landed here!
				if (n == SOCKET_ERROR)
				{
					LPVOID lpMsgBuf;
					FormatMessage( 
						FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
						NULL, WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
						(LPTSTR) &lpMsgBuf, 0, NULL  );
					debug1("WSAGetLastError reports %s", (LPCTSTR)lpMsgBuf);
					LocalFree( lpMsgBuf );
				}
				else
				{
					printf("n = %d\n", n);
				}
#endif /* _DEBUG*/
			break;	// recv failed
		}
	} while((int) ch);

	return strdup(Reply);
}


///////////////////////////////////////////////////////////////////////////////
// HttpRepositoryImpl
///////////////////////////////////////////////////////////////////////////////

HttpRepositoryImpl::HttpRepositoryImpl(
		RepositoryImplFactory &factory, DMINT32 implId, Repository &repository,
		const char *host, int port, const char *uri, const char *params, const char *version
	)
	: RepositoryImpl(factory, implId, repository),
	  m_host(DUP_NULL(host)), m_port(port), m_uri(DUP_NULL(uri)),
	  m_params(DUP_NULL(params)),m_version(DUP_NULL(version))
{
	debug1("HttpRepositoryImpl('%s', %d, '%s', '%s')",
		NULL_CHECK(m_host), m_port, NULL_CHECK(m_uri), NULL_CHECK(m_params));
}


HttpRepositoryImpl::~HttpRepositoryImpl()
{
	SAFE_FREE(m_host);
	SAFE_FREE(m_uri);
	SAFE_FREE(m_params);
	SAFE_FREE(m_version);
}


/**
 * Very simple test to make sure that the repository is correctly configured
 * with all the mandatory properties.
 */
bool HttpRepositoryImpl::testProvider(class Context &ctx)
{
	if(!m_host) {
		ctx.writeToStdErr("\"host\" property must be specified");
		return false;
	}

	return true;
}

static bool local_dirExists(const char *dirname)
{
	struct stat sb;
	if(stat(dirname, &sb) != -1) {
		if(sb.st_mode & STAT_IS_DIRECTORY) {
			return true;
		}
	}
	return false;
}

static char *local_dirname(const char *filename)
{
	const char *ls = strrchr(filename, DIR_SEP_CHAR);
	if(!ls) {
		return strdup(DIR_SEP_STR);
	}

	int len = ls - filename;

	char *ret = (char*) malloc(len + 1);
	if (ret) {
		strncpy(ret, filename, len);
		ret[len] = '\0';
	}
	return ret;
}

static int local_mkdir(const char *dirname)
{
	// Creates the local directory and all interim directories
#ifdef WIN32
	int ret = mkdir(dirname);
#else
		int ret = mkdir(dirname,0777);
#endif			

	if(ret != 0) {
		// mkdir failed - check to see if parent directory exists
		ConstCharPtr parentdir = local_dirname(dirname);
		if((strlen(parentdir) > 0) && (strcmp(parentdir, DIR_SEP_STR) != 0) && !local_dirExists(parentdir)) {
			if (local_mkdir(parentdir)) {
#ifdef WIN32				
				ret = mkdir(dirname);
#else
				ret = mkdir(dirname,0777);
#endif								
			}
		}
	}
	return (ret==0)?true:false;
}

void createDropZoneFile(void *buf,int buflen,const char *path,const char *BaseName)
{
	struct stat s;
	debug1("path=[%s]",path);
	debug1("BaseName=[%s]",BaseName);
	if (stat(path,&s) != 0 && errno == ENOENT) {
		// target path does not exist - create it
		local_mkdir(path);
	}
	char *fp = (char *)malloc(strlen(path)+strlen(BaseName)+10);
	sprintf(fp,"%s/%s",path,BaseName);
	int f = open(fp,_O_WRONLY | _O_BINARY | _O_CREAT | _O_TRUNC, FILE_CREATE_MODE );
	debug1("About to write %d bytes",buflen);
	int res = write(f,buf,buflen);
	debug1("res=%d",res);
	close(f);
}

void *getFileContent(Context &ctx,char *http_request,int sock,int *retlen, int *status)
{
	char *content = (char *)0;
	char *buf = (char *)malloc(strlen(http_request)+50);
	sprintf(buf,"%s\r\n\r\n",http_request);

	SendDataToSocket(sock, buf, strlen(buf));
	free(buf);

	char *line = ReadLineFromSocket(sock);
	debug1("http line read = %s", line);

	if(strncmp(line, "HTTP/1.", 7) != 0) {
		char *line2 = ReadLineFromSocket(sock);
		debug1("http line read 2= %s", line2);

		debug1("CLOSESOCKET");
		CLOSESOCKET(sock);
		return NULL;
	}

	*status = atoi(&line[9]);
	debug1("Status: %d", status);

	if (*status >= 200 && *status <= 299) {
		// Valid return status (got the file)

		long length = -1;
		bool CheckForHTML=false;

		while(true) {
			line = ReadLineFromSocket(sock);
			debug1("http line read (2) = %s", line);

			if(!*line) break;

			if(strncmp(line, "Content-Length: ", 16) == 0) {
				length = atol(&line[16]);
			} else if(strncmp(line, "Content-Type: ", 14) == 0) {
				debug1("Content-Type: %s",&line[14]);
				if (stristr(line,"/html")==0) CheckForHTML=true;
			} 
		}
		debug1("Length: %d", length);

		if(length > 0) {
			char *data = (char*) malloc(length + 1);
			if(ReadDataFromSocket(sock, data, length)) {
				//debug1("Data: %s", data);
				data[length] = '\0';
				content = data;
				*retlen = length;
			}
		} else if(length == -1) {
			content = ReadToEndOfStream(sock,retlen,CheckForHTML);
		}
	} else {
		// No data returned
		ctx.dm().writeToStdErr("%s returns status %d",http_request,*status);
		*retlen = 0;
	}
	return (void *)content;
}

int ConnectToServer(char *host,int port)
{
	struct hostent *hp = gethostbyname(host);
	int sock;
	if(!hp)
	{
		// gethostbyname fails	
		errno = h_errno;
		throw RuntimeError("Failed to resolve webserver name");
	}
	else
	{
		// Create a socket on which to send.
		sock = socket(AF_INET, SOCK_STREAM, 0);		
	}
	struct sockaddr_in	name;
	name.sin_family = AF_INET;
	name.sin_addr.s_addr = *((int *)hp->h_addr);
	name.sin_port = htons(port);	
	
	int res = connect(sock,(struct sockaddr*) &name, sizeof(name));
	if(res)
	{
		// Connection failure
#ifdef WIN32
		errno = WSAGetLastError();
#endif /*WIN32*/
		throw RuntimeError("Failed to connect to webserver");
	}
	return sock;
}

void HttpRepositoryImpl::FreeDirectoryContent(char **dirlist, int numElements)
{
	for (int i=0;i<numElements;i++) {
		SAFE_FREE(dirlist[i]);
	}
	SAFE_FREE(dirlist);
}

char **HttpRepositoryImpl::GetDirectoryContent(Context &ctx,int sock,char *AuthenticationString,char *offset,int *numFiles)
{
	char **ret = (char **)0;
	char *content = (char *)0;
	char *op = (char *)malloc(strlen(AuthenticationString)+strlen(offset)+100);
	sprintf(op,"GET %s%s/ HTTP/1.1\r\nHost: %s%s",offset[0]=='/'?"":"/",offset, m_host,AuthenticationString);
	debug1(op);
	int retlen,status;
	void *data = getFileContent(ctx,op,sock,&retlen,&status);
	debug1("retlen=%d data=[%s]",retlen,(char *)data);
	//
	// We should now have an HTML representation of the directory content
	// <li><a href="DeploymentManager.exe"> DeploymentManager.exe</a></li>
	// <li><a href="FilePublish.jsp"> FilePublish.jsp</a></li>
	// <li><a href="HC_DB2.zip"> HC_DB2.zip</a></li>
	// We have to be tolerant - various webservers may present this list
	// in a different way. But if we take the href that SHOULD be a
	// downloadable file that we can use.
	//
	*numFiles=0;
	for (char *p=(char *)data;p && *p;p++) {
		if (strncmp(p,"href=\"",6)==0) {
			// Found an href
			char *fn = p + 6;				// First char of file name
			char *ep = strchr(fn,'"');		// Get end of file
			if (ep) *ep='\0';				// terminate it
			char *filename = strdup(fn);	// Duplicate the file name
			(*numFiles)++;						// Increment File Count
			ret = (char **)(ret?realloc(ret,(*numFiles)*sizeof(char *)):malloc((*numFiles)*sizeof(char *)));
			ret[(*numFiles)-1] = filename;
			p=ep+1;							// Start again after closing quotes
		}
	}
	return ret;
}

bool HttpRepositoryImpl::CheckFileExists(int sock,char *AuthenticationString,char *offset, char *patt)
{
	char *op = (char *)malloc(strlen(offset)+strlen(patt)+100);
	sprintf(op,"HEAD %s%s/%s HTTP/1.1\r\nHost: %s%s\r\n\r\n",offset[0]=='/'?"":"/",offset, patt, m_host,AuthenticationString);
	debug1(op);
	SendDataToSocket(sock, op, strlen(op));
	char *line = ReadLineFromSocket(sock);
	debug1("http line read = [%s]", line);
	if(strncmp(line, "HTTP/1.", 7) != 0) {
		debug1("CLOSESOCKET");
		CLOSESOCKET(sock);
		return NULL;
	}
	int status = atoi(&line[9]);
	debug1("Status: %d", status);
	line = ReadLineFromSocket(sock);
	while (strlen(line)) {
		line = ReadLineFromSocket(sock);
	}
	return (status>=200 && status <=299);	// success range
}

void http_handleCredentials(
	ExtendedStmt &stmt, Repository &repo, Credentials *&creds, Context &ctx)
{
	creds = repo.getCredentials();
	if(!creds) return;		// No credentials specified

	if (creds->kind() == CREDENTIALS_HARVEST_DFO_IN_FILESYSTEM) {
		// Harvest DFO file given - this is a problem for HTTP servers
		throw RuntimeError(stmt, ctx.stack(),
				"Harvest DFO file cannot be used for HTTP repository");
	}

	if (creds->kind() == CREDENTIALS_PRIVATE_KEY) {
		// Harvest DFO file given - this is a problem for HTTP servers
		throw RuntimeError(stmt, ctx.stack(),
				"Private Key file cannot be used for HTTP repository");
	}
	
	if(!(creds->hasUsername() && creds->hasPassword())) {
		throw RuntimeError(stmt, ctx.stack(),
				"No usable credentials specified for HTTP repository");
	}
}

void HttpRepositoryImpl::checkout(
	const char *dzbase, const char *dzpath,
	bool recursive, StringList *pattern,
	ExtendedStmt &stmt, DropzoneCallback &callback,
	Context &ctx)
{
	// Gets a file from an HTTP server

	Credentials *creds = NULL;
	http_handleCredentials(stmt,m_repository,creds,ctx);
	char *AuthenticationString;
	if (creds) {
		// Credentials Provided - create a base64 encoded username/password pair
		char *username = creds->getDecryptedUsername(ctx);
		char *password = creds->getDecryptedPassword(ctx);
		char *precode = (char *)malloc(strlen(username)+strlen(password)+5);
		sprintf(precode,"%s:%s",username,password);
		char *b64code = base64encode((unsigned char*) precode, strlen(precode));
		AuthenticationString = (char *)malloc(strlen(b64code)+128);
		sprintf(AuthenticationString,"\r\nAuthorization: Basic %s",b64code);
		free(username);
		free(password);
		free(precode);
		free(b64code);
	} else {
		AuthenticationString=(char *)malloc(1);
		AuthenticationString[0]='\0';
	}

	int	sock;
	int	res;

	char *op;
	int retlen,status;
	char *offset = m_uri?m_uri:(char *)"";
	debug1("CHECKOUT: m_uri is %s",m_uri?m_uri:"null");
	if (m_params) {
		// Params go on the uri for GET
		sock = ConnectToServer(m_host,m_port);
		op = (char *)malloc(strlen(AuthenticationString)+strlen(offset)+strlen(m_params)+100);
		char *enc_offset = url_encode(offset);
		char *enc_params = url_encode(m_params);
		sprintf(op,"GET %s?%s HTTP/1.1\r\nHost: %s%s",enc_offset, enc_params, m_host, AuthenticationString);
		debug1(op);
		free(enc_offset);
		free(enc_params);
		void *buf = getFileContent(ctx,op,sock,&retlen,&status);
		StringListIterator iter(*pattern);
		char *BaseName = (char *)iter.first();
		if (BaseName) {
			// The first pattern is the output file from the URL
			createDropZoneFile(buf,retlen,dzpath,BaseName);
			callback.checked_out_file(this, offset, dzpath, "1");	// need to take version from parameter
		}
		free(op);
		free(buf);
	} else {
		// No params - let's take the file pattern
		if (pattern) {
			StringListIterator iter(*pattern);
			for(const char *patt = iter.first(); patt; patt = iter.next()) {
				debug1("patt=[%s]",patt);
				HttpPattern *hpatt = new HttpPattern((char *)patt);
				if (hpatt->getFieldCount() > 0) {
					//
					// File contains a pattern designed to look for latest version
					//
					// Step 1 - see if we can pull a directory listing from the web server
					// -------------------------------------------------------------------
					//
					sock = ConnectToServer(m_host,m_port);
					int numFiles=0;
					char *latestfile=(char *)0;
					char **filelist = GetDirectoryContent(ctx,sock,AuthenticationString,offset,&numFiles);
					debug1("numFiles=%d",numFiles);
					if (numFiles > 0) {
						// Got a list of files
						debug1("Got a filelist from server");
						for (int i=0;i<numFiles;i++) {
							debug1("filelist[%d]=[%s]",i,filelist[i]);
						}
						// We'll assume that the list returned from the server is in alphabetical order
						// Start with the first file in our pattern
						int sp=0;	// starting point
						char *tfile = hpatt->getTestFilename();	// starting file pattern;
						for (int t=0;t<numFiles;t++) {
							if (strcmp(filelist[t],tfile)<0) {
								debug1("Skipping file [%s] in future",filelist[t]);
								sp++;	// Skip this file in future
							}
						}
						SAFE_FREE(tfile);

						int f=1;
						while (f<=hpatt->getFieldCount()) {
							// For each field in the pattern....
							char *tfile = hpatt->getTestFilename();
							debug1("Looking for file [%s]",tfile);
							int t=sp;
							int cfv = -1;
							while (t<numFiles) {
								// debug1("Comparing [%s] and [%s]",filelist[t],tfile);
								if (strcmp(filelist[t],tfile)==0) {
									//debug1("MATCH");
									// Found a file that matches the current "test" filename
									SAFE_FREE(latestfile);
									latestfile = strdup(tfile);
									cfv = hpatt->getFieldValue(f);	// Current Field Value
									// Increment the current field
									debug1("Incrementing field %d",f);
									if (!hpatt->incrField(f)) {
										if (f == hpatt->getFieldCount()) break;
									}
									t=sp;					// reset to start of list
									SAFE_FREE(tfile);
									tfile = hpatt->getTestFilename();
								} else {
									t++;	// next file
								}
							}
							if (cfv > -1) {
								//debug1("Resetting field %d to %d",f,cfv);
								hpatt->setField(f,cfv);	// Reset to last "working" value
								f++;	// move to next field
							} else {
								// Never found anything that matched this pattern.
								// Increment the field and go round again
								if (!hpatt->incrField(f)) f++;
							}
							SAFE_FREE(tfile);
						}
					} else {
						//
						// Could not retrieve an index of files Drop back to slower
						// binary-chop algorithm.
						//
						
						for (int f=1;f<=hpatt->getFieldCount();f++) {
							hpatt->setMidPoint(f);
							int lfv = hpatt->getFieldValue(f);
							while (hpatt->getRange(f)>1) {
								char *tfile = hpatt->getTestFilename();
								// Check to see if this file exists on the server
								//debug1("Checking [%s]",tfile);
								bool fe = CheckFileExists(sock,AuthenticationString,offset,tfile);
								if (fe) {
									// This file exists - move into the top block and try again
									lfv = hpatt->getFieldValue(f);
									SAFE_FREE(latestfile);
									latestfile = strdup(tfile);
									hpatt->chop(f,false);
									debug1("File exists - move up to next block (field %d) lfv=%d",f,lfv);
								} else {
									// This file does not exist - move into the bottom block and try again
									debug1("File does not exist - move to lower block (field %d)",f);
									hpatt->chop(f,true);
								}
							}
							debug1("Setting field %d to %d",f,lfv);
							hpatt->setField(f,lfv);
						}
					}
					debug1("** LATEST FILE: %s",latestfile);
					if (latestfile) {
						op = (char *)malloc(strlen(AuthenticationString)+strlen(offset)+strlen(latestfile)+strlen(m_host)+100);
						char *enc_offset = url_encode(offset);
						char *enc_lf = url_encode(latestfile);
						sprintf(op,"GET %s%s/%s HTTP/1.1\r\nHost: %s%s", offset[0]=='/'?"":"/",enc_offset, enc_lf, m_host,AuthenticationString);
						debug1(op);
						free(enc_offset);
						free(enc_lf);
						callback.checked_out_folder(offset[0]?offset:"/", "\\", true);
						void *buf = getFileContent(ctx,op,sock,&retlen,&status);
						debug1("retlen=%d",retlen);
						// CLOSESOCKET(sock);
						createDropZoneFile(buf,retlen,dzpath,latestfile);

						//
						// Since we can only grab one file at a time from http protocol and since
						// there is no recursion, we don't have to worry about folders.
						//
						char *repopath = (char *)malloc(strlen(offset)+strlen(latestfile)+10);
						sprintf(repopath,"%s%s%s",offset,offset[0]?"/":"",latestfile);
						char *versionString = hpatt->getVersionString();
						callback.checked_out_file(this, repopath, latestfile, versionString);
						SAFE_FREE(versionString);
						callback.checkout_summary(1,1,0,0);						// Record 1 of 1 files checked out successfully
						free(repopath);
						free(op);
						free(buf);
					}
					debug1("CLOSESOCKET");
					CLOSESOCKET(sock);
					// FreeDirectoryContent(filelist,numFiles);
				} else {
					// No fields in pattern - just grab the specified file(s)
					for(const char *patt = iter.first(); patt; patt = iter.next()) {
						sock = ConnectToServer(m_host,m_port);
						op = (char *)malloc(strlen(AuthenticationString)+strlen(offset)+strlen(patt)+strlen(m_host)+100);
						char *enc_offset = url_encode(offset);
						char *enc_patt = url_encode((char *)patt);
						sprintf(op,"GET %s%s/%s HTTP/1.1\r\nHost: %s%s",offset[0]=='/'?"":"/",enc_offset, enc_patt, m_host, AuthenticationString);
						debug1("%s",op);
						free(enc_offset);
						free(enc_patt);
						callback.checked_out_folder(offset[0]?offset:"/", "\\", true);
						void *buf = getFileContent(ctx,op,sock,&retlen,&status);
						debug1("retlen=%d",retlen);
						debug1("status=%d",status);
						CLOSESOCKET(sock);
						if (status>=200 && status <=299) {
							// Success
							createDropZoneFile(buf,retlen,dzpath,patt);
							//
							// Since we can only grab one file at a time from http protocol and since
							// there is no recursion, we don't have to worry about folders.
							//
							char *repopath = (char *)malloc(strlen(offset)+strlen(patt)+10);
							sprintf(repopath,"%s%s%s",offset,offset[0]?"/":"",patt);
							callback.checked_out_file(this, repopath, patt, m_version?m_version:"0");	// need to take version from parameter
							callback.checkout_summary(1,1,0,0);						// Record 1 of 1 files checked out successfully
							free(repopath);
						} else {
							// Failure
							callback.checkout_summary(0,0,0,0);						// Record 0 of 0 files checked out successfully
						}
						
						free(op);
						free(buf);
					}
				}
			}	/* end loop through patterns */
		} /* pattern available */		
	} /* no params for http get */
}


void HttpRepositoryImpl::checkin(
	const char *dzbase, const char *dzpath, ExtendedStmt &stmt,
	DropzoneCallback &callback, Context &ctx)
{}


char *HttpRepositoryImpl::dirname(const char *filename)
{
	const char *ls = strrchr(filename, '\\');
	if(!ls) {
		return NULL;
	}

	int len = ls - filename;

	char *ret = (char*) malloc(len + 1);
	strncpy(ret, filename, len);
	ret[len] = '\0';
	return ret;
}


Expr *HttpRepositoryImpl::getAttribute(const char *name, Context &ctx)
{
	if(strcmp(name, "host") == 0) {
		return new Expr(m_host);
	} else if(strcmp(name, "port") == 0) {
		//char portstr[32];
		//sprintf(portstr, "%d", m_port);
		//return m_port ? strdup(portstr) : NULL;
		return m_port ? new Expr(m_port) : NULL;
	} else if(strcmp(name, "uri") == 0) {
		return new Expr(m_uri);
	}

	return RepositoryImpl::getAttribute(name, ctx);
}


///////////////////////////////////////////////////////////////////////////////
// HttpRepositoryImplFactory
///////////////////////////////////////////////////////////////////////////////

HttpRepositoryImplFactory::HttpRepositoryImplFactory()
{}


RepositoryImpl *HttpRepositoryImplFactory::create(
	DMINT32 implId, Repository &parent, ExtendedStmt &stmt, Context &ctx)
{
	bool ssl=false;
	int sp=0;
	
	ConstCharPtr server  = stmt.getOverridableArgAsString("server", parent, true, ctx);
	ConstCharPtr portstr = stmt.getOverridableArgAsString("port", parent, false, ctx);
	ConstCharPtr params  = stmt.getOverridableArgAsString("params", parent, false, ctx);
	ConstCharPtr version = stmt.getOverridableArgAsString("version", parent, false, ctx);
	ConstCharPtr uri2    = stmt.getOverridableAppendableArgAsString("URI",'/','/',parent,false,ctx);

	debug2("uri2=[%s]",(const char *)uri2);

	if (
		tolower(server[0])=='h' &&
		tolower(server[1])=='t' &&
		tolower(server[2])=='t' &&
		tolower(server[3])=='p') {
			ssl = (tolower(server[4])=='s');
			sp=ssl?6:5;
	}
	const char *t = (const char *)server;
	char *host = strdup(sp>0?&(t[sp+2]):t);
	int port = ssl?443:80; // default
	char *uri1 = strchr(host,'/');
	char *uri = (char *)0;
	if (uri1) {
		*uri1='\0';
		uri1++;
		// uri = (char *)malloc(strlen(uri1)+1);
		// strcpy(uri,uri1);
		uri = strdup(uri1);
	}
	if (uri2) {
		uri = (char *)(uri?realloc(uri,strlen(uri)+strlen(uri2)+2):malloc(strlen(uri2)+1));
		if (uri1) {
			strcat(uri,"/");
		} else {
			uri[0]='\0';
		}
		strcat(uri,uri2);
	}
	
	char *cp = strchr(host,':');
	if (cp) {
		// url contains a : after the hostname, port specified
		port=atoi(cp+1);
		*cp='\0';	// terminate hostname before the port number
	}
	
	if(portstr && portstr[0]) {
		port = atoi(portstr);
	}
	if (params && !params[0]) params = NULL;

	return new HttpRepositoryImpl(*this, implId, parent, host, port, uri, params, version);
}


///////////////////////////////////////////////////////////////////////////////


extern "C" HTTP_EXPORT int PluginStart(DM &dm)
{
	dm.writeToLogFile("Http plugin V" HTTP_PLUGIN_VERSION);
	
	RepositoryImplRegistry::instance()
		.registerFactory(HTTP_REPO_NAME, new HttpRepositoryImplFactory());

	return 0;
}


extern "C" HTTP_EXPORT int PluginInstall(DM &dm)
{
	// name, required, appendable
	List<PropertyDef> *propdefs = new List<PropertyDef>(true);
	propdefs->add(new PropertyDef("server", true, true ));
	propdefs->add(new PropertyDef("port", false, false));
	propdefs->add(new PropertyDef("params", false, true));
	propdefs->add(new PropertyDef("filename", false, false));
	propdefs->add(new PropertyDef("URI", false, true));

	dm.installProviderImpl(HTTP_REPO_NAME, HTTP_PLUGIN_NAME, OBJ_KIND_REPOSITORY, propdefs);

	return 0;
}