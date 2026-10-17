#pragma once
#include "raw_can_journal.h"

namespace m5can {
// Returns an existing journal even if the CANLOG partition is missing.
// initializeRawCanJournal() fails closed unless a custom unencrypted 'canlog'
// partition exists and is fully blank or already owned by our journal.
RawCanJournal& rawCanJournal();
bool initializeRawCanJournal();
} // namespace m5can
