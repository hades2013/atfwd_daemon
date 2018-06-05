/*!
  @file
  atfwd_daemon.c

  @brief
  ATFWD daemon which registers with QMI ATCOP service and forwards AT commands

*/

/*===========================================================================

  Copyright (c) 2012 Qualcomm Technologies, Inc. All Rights Reserved

  Qualcomm Technologies Proprietary

  Export of this technology or software is regulated by the U.S. Government.
  Diversion contrary to U.S. law prohibited.

  All ideas, data and information contained in or disclosed by
  this document are confidential and proprietary information of
  Qualcomm Technologies, Inc. and all rights therein are expressly reserved.
  By accepting this material the recipient agrees that this material
  and the information contained therein are held in confidence and in
  trust and will not be used, copied, reproduced in whole or in part,
  nor its contents revealed in any manner to others without the express
  written permission of Qualcomm Technologies, Inc.

===========================================================================*/

/*===========================================================================

                           INCLUDE FILES

===========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <qmi_atcop_srvc.h>
#include <qmi.h>
#include <string.h>
#include <cutils/properties.h>
#include <errno.h>
#include <fcntl.h>
#define LOGI(...) fprintf(stderr, "I:" __VA_ARGS__)
#define MAX_DIGITS 10
#define DEFAULT_QMI_PORT QMI_PORT_RMNET_SDIO_0
#define DEFAULT_SMD_PORT QMI_PORT_RMNET_0

#define ATFWD_DATA_PROP_SIZE 16

/* name of pipe to write to */
#define REBOOT_PIPE "/dev/rebooterdev"

/*qmi message library handle*/
int qmi_handle = QMI_INVALID_CLIENT_HANDLE;
typedef struct {
    int opcode;
    char *name;
    int ntokens;
    char **tokens;
} AtCmd;

typedef struct {
    int result;
    char *response;
}AtCmdResponse;

/*===========================================================================

                           Global Variables

===========================================================================*/

int userData; //Extra user data sent by QMI
int qmiErrorCode; //Stores the QMI error codes
int userHandle; //Connection ID
qmi_atcop_abort_type abortType; //AT command abort type
qmi_atcop_at_cmd_fwd_req_type atCmdFwdReqType[] = {
    { //AT command fwd type
        1, // Number of commands
        {
            { QMI_ATCOP_AT_CMD_NOT_ABORTABLE, "+CFUN"},
        }
    },
    { //AT command fwd type
        1, // Number of commands
        {
            { QMI_ATCOP_AT_CMD_NOT_ABORTABLE, "$QCPWRDN"},
        }
    },
};

qmi_atcop_at_cmd_hndl_type commandHandle; //ATFWD request handle
qmi_atcop_at_cmd_fwd_ind_type request; //Input request string

//qmi_atcop_fwd_resp_status_type responseStatus; //ATFWD response status
qmi_atcop_fwd_resp_result_type responseResult; //ATFWD response result
qmi_atcop_fwd_resp_response_type responseType; //ATFWD response type
qmi_atcop_fwd_resp_at_resp_type atCmdResponse; //Actual ATFWD response

AtCmd fwdcmd;
AtCmdResponse fwdresponse;

pthread_cond_t ctrCond; //Condition variable that will be used to indicate if a request has arrived
pthread_mutex_t ctrMutex; //Mutex that will be locked when a request is processed
int newRequest = 0; //New request indication
int atResponseStatus = 0;
char *atCmd;
/* string to send to reboot_daemon to reboot */
const char* REBOOT_STR = "REBOOT\0";

typedef struct atfwd_sig_handler_s
{
  int sig;
  __sighandler_t handler;
} atfwd_sig_handler_t;

/* All termination SIGNALS except SIGKILL
 * SIGKILL cannot be handled or ignored
*/
atfwd_sig_handler_t atfwd_sig_handler_tbl[] =
{
  {SIGTERM, NULL},
  {SIGINT, NULL},
  {SIGQUIT, NULL},
  {SIGHUP, NULL}
};

/*===========================================================================
  FUNCTION  parseInput
===========================================================================*/
/*!
@brief
  Parses the input request string and populates the AtCmd struct with the
  data to forward

@return
  None

*/
/*=========================================================================*/
void parseInput()
{
    int i;
    fwdcmd.opcode = request.op_code;
    fwdcmd.name = strdup((char *)request.at_name);
    fwdcmd.ntokens = request.num_tokens;
    fwdcmd.tokens = calloc(request.num_tokens, sizeof(char *));
    if(NULL != fwdcmd.tokens) {
        for (i = 0; i < request.num_tokens; i++) {
            fwdcmd.tokens[i] = strdup((char *)request.tokens[i]);
        }
    }
    if(atCmd)
        free(atCmd);
     if (fwdcmd.name){
         atCmd = strdup(fwdcmd.name);
     }
    // We only support +CFUN=1,1 for now
    if (fwdcmd.name ) {
        if(!strncasecmp(fwdcmd.name, "+CFUN", strlen("+CFUN")) && fwdcmd.ntokens == 2) {
        if (fwdcmd.tokens && fwdcmd.tokens[0] && fwdcmd.tokens[1]) {
            if (!strcmp(fwdcmd.tokens[0], "1") && !strcmp(fwdcmd.tokens[1], "1")) {
                LOGI("Going into reboot\n");
                atResponseStatus = QMI_ATCOP_RESULT_OK;
                return;
            }
        }
    } else if (!strncasecmp(fwdcmd.name, "$QCPWRDN", strlen("$QCPWRDN"))) {
            LOGI("Going to halt\n");
            atResponseStatus = QMI_ATCOP_RESULT_OK;
            return;
        }
    atResponseStatus = QMI_ATCOP_RESULT_ERROR;
    LOGI("Unsupported AT-Command / tokens.\n", fwdcmd.name);
    }

}

/*===========================================================================
  FUNCTION  exitDaemon
===========================================================================*/
/*!
@brief
  Utility method which handles when user presses CTRL+C

@return
  None

@note
  None
*/
/*=========================================================================*/
void exitDaemon(int sig)
{
    LOGI("Going to kill ATFWD daemon\n");
    (void)sig;
    unsigned int i=0;
    /* Note that the handler should ignore all the reg. signals
     * because they do not want to be interfered
     * while an ongoing signal is being processed
     */
    for(i=0; i<sizeof(atfwd_sig_handler_tbl)/sizeof(atfwd_sig_handler_t); i++) {
        signal(atfwd_sig_handler_tbl[i].sig, SIG_IGN);
    }
    int clientRelease = qmi_atcop_srvc_release_client (userHandle, &qmiErrorCode);
    if (clientRelease < 0) {
        LOGI("QMI client release error: %d\n", qmiErrorCode);
    }
    if (qmi_handle >= 0) {
        qmi_release(qmi_handle);
    }
    pthread_cond_destroy(&ctrCond);
    pthread_mutex_destroy(&ctrMutex);

    for(i=0; i<sizeof(atfwd_sig_handler_tbl)/sizeof(atfwd_sig_handler_t); i++) {
        LOGI("\natfwd_sig_handler_tbl[i].sig : %d", atfwd_sig_handler_tbl[i].sig);
        if (atfwd_sig_handler_tbl[i].sig == sig &&
            atfwd_sig_handler_tbl[i].handler != NULL) {
            /* call  default installed handler */
            LOGI("\ncall default handler [%p] for sig [%d]",
                  atfwd_sig_handler_tbl[i].handler,
                  atfwd_sig_handler_tbl[i].sig);
            (atfwd_sig_handler_tbl[i].handler)(sig);
            break;
        }
    }
    exit(0);
}

/*===========================================================================
  FUNCTION:  signal_init
===========================================================================*/
/*!
    @brief
    Signal specific initialization

    @return
    void
*/
/*=========================================================================*/
void signal_init(void)
{
    unsigned int i=0;
    __sighandler_t temp;

    for(i=0; i<sizeof(atfwd_sig_handler_tbl)/sizeof(atfwd_sig_handler_t); i++) {
        temp = atfwd_sig_handler_tbl[i].handler;
        atfwd_sig_handler_tbl[i].handler = signal(atfwd_sig_handler_tbl[i].sig,
                                                  exitDaemon);
        /* swap previous handler back if signal() was unsuccessful */
        if (SIG_ERR == atfwd_sig_handler_tbl[i].handler) {
            atfwd_sig_handler_tbl[i].handler = temp;
        }
    }
}

/*===========================================================================
  FUNCTION  sendSuccessResponse
===========================================================================*/
/*!
@brief
  Sends OK response to QMI.

@return
  None

@note
  None
*/
/*=========================================================================*/
void sendSuccessResponse()
{
    //responseStatus = QMI_ATCOP_SUCCESS;
    responseResult = QMI_ATCOP_RESULT_OK;
    responseType = QMI_ATCOP_RESP_COMPLETE;
    atCmdResponse.at_hndl = commandHandle;
    //atCmdResponse.status = responseStatus;
    atCmdResponse.result = responseResult;
    atCmdResponse.response = responseType;
    atCmdResponse.at_resp = NULL;
    if (qmi_atcop_fwd_at_cmd_resp(userHandle, &atCmdResponse, &qmiErrorCode) < 0) {
        LOGI("QMI response error: %d\n", qmiErrorCode);
    }
}

/*===========================================================================
  FUNCTION  sendResponse
===========================================================================*/
/*!
@brief
  Sends response to QMI.

@return
  None

@note
  None
*/
/*=========================================================================*/
void sendResponse(AtCmdResponse *response)
{
    if (!response) {
        LOGI("Have null response");
        return;
    }
    //responseStatus = QMI_ATCOP_SUCCESS;
    responseResult = response->result;
    responseType = QMI_ATCOP_RESP_COMPLETE;
    atCmdResponse.at_hndl = commandHandle;
    //atCmdResponse.status = responseStatus;
    atCmdResponse.result = responseResult;
    atCmdResponse.response = responseType;
    if (request.cmee_val == 0) {
        atCmdResponse.at_resp = NULL;
    } else {
        char *msg = NULL;
        unsigned long s3 = request.s3_val;
        unsigned long s4 = request.s4_val;
        if (response->response && response->response[0]) {
            // Need space for the end of line and carriage return chars (S3/S4)
            size_t l = 4*4 + strlen(response->response) + 1;
            msg = malloc(l);
            if(NULL != msg)
                snprintf(msg, l, "%lc%lc%s%lc%lc", s3, s4, response->response, s3, s4);
        }
        atCmdResponse.at_resp = (unsigned char *)msg;
    }
    if (qmi_atcop_fwd_at_cmd_resp(userHandle, &atCmdResponse, &qmiErrorCode) < 0) {
        LOGI("QMI response error: %d\n", qmiErrorCode);
    }
    if(NULL != atCmdResponse.at_resp) {
        free(atCmdResponse.at_resp);
        atCmdResponse.at_resp = NULL;
    }
}

/*===========================================================================
  FUNCTION  sendInvalidCommandResponse
===========================================================================*/
/*!
@brief
  Sends ERROR response to QMI.

@return
  None

@note
  None
*/
/*=========================================================================*/
void sendInvalidCommandResponse()
{
    responseResult = QMI_ATCOP_RESULT_ERROR;
    responseType   = QMI_ATCOP_RESP_COMPLETE;

    atCmdResponse.at_hndl  = commandHandle;
    atCmdResponse.result   = responseResult;
    atCmdResponse.response = responseType;

    if (request.cmee_val == 0) {
        atCmdResponse.at_resp = NULL;
    } else {
        char *response;
        char s3Val[MAX_DIGITS];
        char s4Val[MAX_DIGITS];

        snprintf(s3Val, MAX_DIGITS, "%lc", request.s3_val);
        snprintf(s4Val, MAX_DIGITS, "%lc", request.s4_val);

        size_t respLen  = ((strlen(s3Val) * 2) + (strlen(s4Val) * 2) + 13 + 1) * sizeof(char);
        response = (char *)malloc(respLen);
        if (!response) {
            LOGI("No memory for generating invalid command response\n");
            atCmdResponse.at_resp = NULL;
        } else {
            snprintf(response, respLen, "%s%s+CME ERROR :2%s%s", s3Val, s4Val, s3Val, s3Val);
            atCmdResponse.at_resp = (unsigned char *)response;
        }
    }

    if (qmi_atcop_fwd_at_cmd_resp(userHandle, &atCmdResponse, &qmiErrorCode) < 0) {
        LOGI("QMI response error: %d\n", qmiErrorCode);
    }

    if (atCmdResponse.at_resp) {
        free(atCmdResponse.at_resp);
        atCmdResponse.at_resp = NULL;
    }
}

/*===========================================================================
  FUNCTION  sendCommand
  ===========================================================================*/
/*!
  @brief
  Routine that will be invoked by QMI upon a request for AT command. It
  checks for the validity of the request and finally spawns a new thread to
  process the key press events.

  @return
  None

  @note
  None

*/
/*=========================================================================*/
static void atCommandCb(int userHandle, qmi_service_id_type serviceID,
        void *userData, qmi_atcop_indication_id_type indicationID,
        qmi_atcop_indication_data_type  *indicationData)
{
    LOGI("atCommandCb\n");

    /* Check if it's an abort request */
    if (indicationID == QMI_ATCOP_SRVC_ABORT_MSG_IND_TYPE) {
        LOGI("Received abort message from QMI\n");
    } else if (indicationID == QMI_ATCOP_SRVC_AT_FWD_MSG_IND_TYPE) {
        LOGI("Received AT command forward request\n");
        pthread_mutex_lock(&ctrMutex);
        commandHandle = indicationData->at_hndl;
        request = indicationData->at_cmd_fwd_type;
        parseInput();
        newRequest = 1;
        pthread_cond_signal(&ctrCond);
        pthread_mutex_unlock(&ctrMutex);
    }
}

void freeAtCmdResponse(AtCmdResponse *response) {
    if (!response) return;
        if (response->response)
            free(response->response);
    free(response);
}

void freeAtCmd(AtCmd *cmd) {
    int i;

    if (!cmd) return;

    if (cmd->name) free(cmd->name);

    if (cmd->ntokens != 0 && cmd->tokens) {
        for (i = 0; i < cmd->ntokens; i++) {
            free(cmd->tokens[i]);
        }
        free(cmd->tokens);
    }
    free(cmd);
}

/*=========================================================================
FUNCTION:  main

===========================================================================*/
/*!
  @brief
  Initialize the QMI connection and register the ATFWD event listener.
  argv[1] if provided, gives the name of the qmi port to open.
  Default is "rmnet0".

*/
/*=========================================================================*/
int main (int argc, char **argv)
{
    AtCmdResponse *response = NULL;
    int i, nErrorCnt = 0;
    const char *qmi_port = NULL;

    userHandle = -1;

    if (argc >= 2) {
        qmi_port = argv[1];
    } else {
        qmi_port = DEFAULT_SMD_PORT;
    }

    LOGI("ATFWD --> QMI Port : %s\n" , qmi_port);
    qmi_handle = qmi_init(NULL, NULL);
    if (qmi_handle < 0) {
        LOGI("Could not initialize qmi message library\n");
        return -1;
    }
    signal_init();
    int connectionResult = qmi_connection_init(qmi_port, &qmiErrorCode);
    if (connectionResult < 0) {
        LOGI("Could not connect with QMI Interface. The QMI error code is %d\n", qmiErrorCode);
        if (qmi_handle >= 0) {
            qmi_release(qmi_handle);
        }
        return -1;
    }
    LOGI("QMI connection init done\n");

    userHandle = qmi_atcop_srvc_init_client(qmi_port, atCommandCb, NULL , &qmiErrorCode);
    if (userHandle < 0) {
        LOGI("Could not register with the QMI Interface. The QMI error code is %d\n", qmiErrorCode);
    }

    if(userHandle < 0 ) {
        LOGI("Could not register userhandle(s) with both 8k and 9k modems -- bail out");
        if (qmi_handle >= 0) {
            qmi_release(qmi_handle);
        }
        return -1;
    }

    LOGI("Registered with QMI with client id %d\n", userHandle);

    pthread_mutexattr_t attr;
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&ctrMutex, &attr);
    pthread_cond_init(&ctrCond, NULL);

    int ncommands = sizeof(atCmdFwdReqType) / sizeof(atCmdFwdReqType[0]);
    LOGI("Trying to register %d commands:\n", ncommands);
    for (i = 0; i < ncommands ; i++) {
        LOGI("cmd %d: %s\n", i, atCmdFwdReqType[i].qmi_atcop_at_cmd_fwd_req_type[0].at_cmd_name);
        qmiErrorCode = 0;
        int registrationStatus = qmi_atcop_reg_at_command_fwd_req(userHandle, &atCmdFwdReqType[i], &qmiErrorCode);
        if (registrationStatus < 0) {
            LOGI("Could not register AT command : %s with the QMI Interface - Err code:%d\n",
                    atCmdFwdReqType[i].qmi_atcop_at_cmd_fwd_req_type[0].at_cmd_name, qmiErrorCode);
            nErrorCnt++;
            qmiErrorCode = 0;
        }
    }

    if(nErrorCnt == ncommands ) {
        LOGI("atfwd : Baling out ");
        qmi_atcop_srvc_release_client(userHandle, &qmiErrorCode);
        if (qmi_handle >= 0) {
            qmi_release(qmi_handle);
        }
        return -1;
    }

    LOGI("Registered AT Commands event handler\n");

    while (1) {
        pthread_mutex_lock(&ctrMutex);
        while (newRequest == 0) {
            pthread_cond_wait(&ctrCond, &ctrMutex);
        }
        response = calloc(1,sizeof(AtCmdResponse));
        if(!response || QMI_ATCOP_RESULT_ERROR == atResponseStatus) {
            sendInvalidCommandResponse();
        } else {
            response->result = atResponseStatus;
            sendResponse(response);
    }

        if (fwdcmd.name) free(fwdcmd.name);
        if (fwdcmd.ntokens != 0 && fwdcmd.tokens) {
            for (i = 0; i < fwdcmd.ntokens; i++) {
                free(fwdcmd.tokens[i]);
            }
            free(fwdcmd.tokens);
        }
        freeAtCmdResponse(response);
        newRequest = 0;
        pthread_mutex_unlock(&ctrMutex);

        // Invoke the handler based on atCmd type
        if(atCmd && atResponseStatus == QMI_ATCOP_RESULT_OK) {
            if(!strncasecmp(atCmd, "+CFUN", strlen("+CFUN"))) {
                system("reboot");
            } else if (!strncasecmp(atCmd, "$QCPWRDN", strlen("$QCPWRDN"))) {
                LOGI("QCPWRDN has been detected");
                /* Halt the UE/APPS */
                system("halt");
            }
        }
        if(atCmd)
            free(atCmd);
    }

    return 0;
}
