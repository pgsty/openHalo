/* Final row accounting for MySQL INSERT ... ON DUPLICATE KEY / REPLACE. */
#include "postgres.h"
#include "executor/tuptable.h"
#include "utils/datum.h"
#include "halo_mysql.h"

uint64
mys_upsert_row(TupleTableSlot *oldslot, TupleTableSlot *newslot, uint64 deleted)
{
	TupleDesc desc = newslot->tts_tupleDescriptor;
	uint64 row_result;

	if (deleted > 0)
	{
		return MYS_ROW_UNCHANGED + deleted;
	}
	row_result = MYS_ROW_INSERTED;
	if (oldslot == NULL)
		return row_result;
	slot_getallattrs(oldslot);
	slot_getallattrs(newslot);
	row_result = MYS_ROW_UNCHANGED;
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
			continue;
		if (oldslot->tts_isnull[i] != newslot->tts_isnull[i] ||
			(!oldslot->tts_isnull[i] &&
			 !datum_image_eq(oldslot->tts_values[i], newslot->tts_values[i],
							 att->attbyval, att->attlen)))
		{
			row_result = MYS_ROW_CHANGED;
			break;
		}
	}
	return row_result;
}
