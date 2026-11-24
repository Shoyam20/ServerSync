
#include "common.h"
using namespace std;

int main() {
    log_msg("ServerSync starting...");

    if (!load_templates(TEMPLATE_DIR)) {    // Pages cannot be built without templates.
        log_msg("Cannot start: templates are missing. Run the server from the project folder (it needs templates/).");
        return 1;
    }
    if (!init_db()) {                   // Without the database the server does not start.
        log_msg("Cannot start without the database. Fix db.conf / MySQL and try again.");
        return 1;
    }
    if (!start_server()) {
        close_db();
        return 1;
    }
    start_logger();     // Starts before the workers so even the first request is logged.
    start_workers();
    start_cleanup();    // Cleanup and delivery threads (both use the database).
    run_server();        // Blocks here until Ctrl+C.
    stop_workers();     // Workers first (they push logs), then the other threads.
    stop_cleanup();
    stop_logger();      // No new logs can arrive now: flush the rest, then stop.
    close_db();         // Last, because the logger, cleanup and delivery threads use the database.
    stop_server();

    log_msg("ServerSync stopped.");
    return 0;
}
