#include "pd_worker_units.h"
#include "pd_unit_files.h"
#include "pd_worker_unit_text.h"
#include <unistd.h>
static const struct pd_unit_files definitions={pd_worker_unit_names,pd_worker_unit_text};
int pd_worker_units_verify(int units)
{
    return pd_unit_files_verify(units,&definitions);
}
int pd_worker_units_arm(int units)
{
    return pd_unit_files_arm(units,&definitions);
}
int pd_worker_units_disarm(int units,int store,const unsigned char plan[PD_MIGRATION_SIZE])
{
    return (pd_transaction_at_phase(store,plan,PD_TRANSACTION_COMMITTED) ||
        pd_transaction_at_phase(store,plan,PD_TRANSACTION_ROLLED_BACK)) && !fsync(store) &&
        pd_unit_files_remove(units,&definitions);
}
