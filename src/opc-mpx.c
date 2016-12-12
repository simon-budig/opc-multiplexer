#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <error.h>

#include <glib.h>
#include <glib-object.h>

#include "opc-types.h"

#include "opcbroker.h"

int verbosity_level = 0;
static GMainLoop *mainloop = NULL;
static gboolean exit_requested = FALSE;

static void
signal_term_handler (int sig)
{
  g_main_loop_quit (mainloop);
  if (sig == SIGTERM)
    exit_requested = TRUE;
}


int
main (int   argc,
      char *argv[])
{
  OpcBroker *broker;
  int ret;

  if (argc > 1 && !strcmp (argv[1], "--debug"))
    verbosity_level = 2;

  broker = opc_broker_new ();

  if (!opc_broker_connect_target (broker,
                                  "localhost:15163",
                                  15163))
    {
      perror ("can't connect to target");
      return -1;
    }

  ret = opc_broker_run (broker,
                        7890,
                        NULL);

  if (ret < 0)
    perror ("opc-mpx run");

  /* mainloop */

  mainloop = g_main_loop_new (NULL, FALSE);

  signal (SIGTERM, signal_term_handler);
  signal (SIGHUP,  signal_term_handler);

  while (!exit_requested)
    {
      g_main_loop_run (mainloop);

      /* after a signal we land here */
      g_printerr ("signal caught!\n");
    }

  return 0;
}
