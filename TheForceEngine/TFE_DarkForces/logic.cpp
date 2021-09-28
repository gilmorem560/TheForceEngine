#include "logic.h"
#include "pickup.h"
#include "player.h"
#include "updateLogic.h"
#include "animLogic.h"
#include "vueLogic.h"
#include "projectile.h"
#include <TFE_DarkForces/Actor/barrel.h>
#include <TFE_DarkForces/Actor/landmine.h>
#include <TFE_DarkForces/Actor/mousebot.h>
#include <TFE_DarkForces/Actor/scenery.h>
#include <TFE_DarkForces/Actor/officer.h>
#include <TFE_Jedi/Level/robject.h>
#include <TFE_Jedi/Memory/allocator.h>

using namespace TFE_Jedi;

namespace TFE_DarkForces
{
	char s_objSeqArg0[256];
	char s_objSeqArg1[256];
	char s_objSeqArg2[256];
	char s_objSeqArg3[256];
	char s_objSeqArg4[256];
	char s_objSeqArg5[256];
	s32  s_objSeqArgCount;

	Logic* obj_setEnemyLogic(SecObject* obj, KEYWORD logicId, LogicSetupFunc* setupFunc);

	void obj_addLogic(SecObject* obj, Logic* logic, Task* task, LogicCleanupFunc cleanupFunc)
	{
		if (!obj->logic)
		{
			obj->logic = allocator_create(sizeof(Logic**));
		}

		Logic** logicItem = (Logic**)allocator_newItem((Allocator*)obj->logic);
		*logicItem = logic;
		logic->obj = obj;
		logic->parent = logicItem;
		logic->task = task;
		logic->cleanupFunc = cleanupFunc;
	}

	void deleteLogicAndObject(Logic* logic)
	{
		if (s_freeObjLock) { return; }

		SecObject* obj = logic->obj;
		Logic** parent = logic->parent;
		Logic** logics = (Logic**)allocator_getHead_noIterUpdate((Allocator*)obj->logic);

		allocator_deleteItem((Allocator*)obj->logic, parent);
		if (parent == logics)
		{
			freeObject(obj);
		}
	}

	JBool logic_defaultSetupFunc(SecObject* obj, KEYWORD key)
	{
		char* endPtr;
		JBool retValue = JTRUE;
		if (key == KW_EYE)
		{
			player_setupEyeObject(obj);
			//setCameraOffset(0, 0, 0);
			//setCameraAngleOffset(0, 0, 0);
		}
		else if (key == KW_BOSS)
		{
			obj->flags |= OBJ_FLAG_BOSS;
		}
		else if (key == KW_HEIGHT)
		{
			obj->worldHeight = floatToFixed16(strtof(s_objSeqArg1, &endPtr));
		}
		else if (key == KW_RADIUS)
		{
			obj->worldWidth = floatToFixed16(strtof(s_objSeqArg1, &endPtr));
			if (obj->worldWidth > 0)
			{
				obj->flags |= OBJ_FLAG_HAS_COLLISION;
			}
		}
		else  // Invalid key.
		{
			retValue = JFALSE;
		}
		return retValue;
	}

	JBool object_parseSeq(SecObject* obj, TFE_Parser* parser, size_t* bufferPos)
	{
		LogicSetupFunc setupFunc = nullptr;

		const char* line = parser->readLine(*bufferPos);
		if (!line || !strstr(line, "SEQ"))
		{
			return JFALSE;
		}

		Logic* newLogic = nullptr;
		while (1)
		{
			line = parser->readLine(*bufferPos);
			if (!line) { return JFALSE; }

			s_objSeqArgCount = sscanf(line, " %s %s %s %s %s %s", s_objSeqArg0, s_objSeqArg1, s_objSeqArg2, s_objSeqArg3, s_objSeqArg4, s_objSeqArg5);
			KEYWORD key = getKeywordIndex(s_objSeqArg0);
			if (key == KW_TYPE || key == KW_LOGIC)
			{
				KEYWORD logicId = getKeywordIndex(s_objSeqArg1);
				if (logicId == KW_PLAYER)  // Player Logic.
				{
					player_setupObject(obj);
					setupFunc = nullptr;
				}
				else if (logicId == KW_ANIM)	// Animated Sprites Logic.
				{
					newLogic = obj_setSpriteAnim(obj);
				}
				else if (logicId == KW_UPDATE)	// "Update" logic is usually used for rotating 3D objects, like the Death Star.
				{
					newLogic = obj_setUpdate(obj, &setupFunc);
				}
				else if (logicId >= KW_TROOP && logicId <= KW_SCENERY)	// Enemies, explosives barrels, land mines, and scenery.
				{
					newLogic = obj_setEnemyLogic(obj, logicId, &setupFunc);
				}
				else if (logicId == KW_KEY)         // Vue animation logic.
				{
					newLogic = obj_createVueLogic(obj, &setupFunc);
				}
				else if (logicId == KW_GENERATOR)	// Enemy generator, used for in-level enemy spawning.
				{
					// TODO(Core Game Loop Release) - come back to this once the AI is fully integrated.
					// Turn off generator collision for now, until this is setup correctly.
					obj->flags &= ~OBJ_FLAG_HAS_COLLISION;
				}
				else if (logicId == KW_DISPATCH)
				{
					// TODO(Core Game Loop Release) - come back to this once the level is running
					// Turn off dispatch collision for now, until this is setup correctly.
					obj->flags &= ~OBJ_FLAG_HAS_COLLISION;
				}
				else if ((logicId >= KW_BATTERY && logicId <= KW_AUTOGUN) || logicId == KW_ITEM)
				{
					if (logicId >= KW_BATTERY && logicId <= KW_AUTOGUN)
					{
						strcpy(s_objSeqArg2, s_objSeqArg1);
					}
					ItemId itemId = getPickupItemId(s_objSeqArg2);
					obj_createPickup(obj, itemId);
					setupFunc = nullptr;
				}
				else
				{
					// TODO(Core Game Loop Release) - figure out why scenery isn't being setup correctly.
					obj->flags &= ~OBJ_FLAG_HAS_COLLISION;
				}
			}
			else if (key == KW_SEQEND)
			{
				return JTRUE;
			}
			else
			{
				if (setupFunc && setupFunc(newLogic, key))
				{
					continue;
				}
				logic_defaultSetupFunc(obj, key);
			}
		}

		return JTRUE;
	}

	Logic* obj_setEnemyLogic(SecObject* obj, KEYWORD logicId, LogicSetupFunc* setupFunc)
	{
		obj->flags |= OBJ_FLAG_ENEMY;

		switch (logicId)
		{
			case KW_TROOP:
			case KW_STORM1:
			{
			} break;
			case KW_INT_DROID:
			{
			} break;
			case KW_PROBE_DROID:
			{
			} break;
			case KW_D_TROOP1:
			{
			} break;
			case KW_D_TROOP2:
			{
			} break;
			case KW_D_TROOP3:
			{
			} break;
			case KW_BOBA_FETT:
			{
			} break;
			case KW_COMMANDO:
			{
			} break;
			case KW_I_OFFICER:
			case KW_I_OFFICER1:
			case KW_I_OFFICER2:
			case KW_I_OFFICER3:
			case KW_I_OFFICER4:
			case KW_I_OFFICER5:
			case KW_I_OFFICER6:
			case KW_I_OFFICER7:
			case KW_I_OFFICER8:
			case KW_I_OFFICER9:
			case KW_I_OFFICERR:
			case KW_I_OFFICERY:
			case KW_I_OFFICERB:
			{
				obj->entityFlags = ETFLAG_AI_ACTOR | ETFLAG_HAS_GRAVITY;
				officer_setup(obj, setupFunc, logicId);
			} break;
			case KW_G_GUARD:
			{
			} break;
			case KW_REE_YEES:
			{
			} break;
			case KW_REE_YEES2:
			{
			} break;
			case KW_BOSSK:
			{
			} break;
			case KW_BARREL:
			{
				obj->entityFlags = ETFLAG_AI_ACTOR;
				barrel_setup(obj, setupFunc);
			} break;
			case KW_LAND_MINE:
			{
				ProjectileLogic* mineLogic = (ProjectileLogic*)createProjectile(PROJ_LAND_MINE_PLACED, obj->sector, obj->posWS.x, obj->posWS.y, obj->posWS.z, obj);
				freeObject(obj);

				SecObject* mineObj = mineLogic->logic.obj;
				mineObj->entityFlags |= ETFLAG_LANDMINE;
				mineObj->projectileLogic = (Logic*)mineLogic;

				landmine_setup(mineObj, setupFunc);
				return (Logic*)mineLogic;
			} break;
			case KW_KELL:
			{
			} break;
			case KW_SEWER1:
			{
			} break;
			case KW_REMOTE:
			{
			} break;
			case KW_TURRET:
			{
			} break;
			case KW_MOUSEBOT:
			{
				return mousebot_setup(obj, setupFunc);
			} break;
			case KW_WELDER:
			{
			} break;
			case KW_SCENERY:
			{
				obj->flags &= ~OBJ_FLAG_ENEMY;
				obj->entityFlags = ETFLAG_SCENERY;
				scenery_setup(obj, setupFunc);
			} break;
		}

		// Fallthrough returns null.
		// TODO: Add an assert or error once all of the cases are handled.
		return nullptr;
	}
}  // TFE_DarkForces