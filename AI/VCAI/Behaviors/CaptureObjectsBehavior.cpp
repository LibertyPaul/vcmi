/*
* CaptureObjectsBehavior.cpp, part of VCMI engine
*
* Authors: listed in file AUTHORS in main folder
*
* License: GNU General Public License v2.0 or later
* Full text of license available in license.txt file, in main folder
*
*/
#include "StdInc.h"
#include "../VCAI.h"
#include "../Engine/Nullkiller.h"
#include "../Goals/Composition.h"
#include "../Goals/ExecuteHeroChain.h"
#include "CaptureObjectsBehavior.h"
#include "../AIUtility.h"
#include "../../../lib/mapping/CMap.h" //for victory conditions
#include "../../../lib/CPathfinder.h"
#include "../../../lib/CModHandler.h"

extern boost::thread_specific_ptr<CCallback> cb;
extern boost::thread_specific_ptr<VCAI> ai;

using namespace Goals;

template <typename T>
bool vectorEquals(const std::vector<T> & v1, const std::vector<T> & v2)
{
	return vstd::contains_if(v1, [&](T o) -> bool
	{
		return vstd::contains(v2, o);
	});
}

std::string CaptureObjectsBehavior::toString() const
{
	return "Capture objects";
}

bool CaptureObjectsBehavior::operator==(const CaptureObjectsBehavior & other) const
{
	if(specificObjects != other.specificObjects)
		return false;

	if(specificObjects)
		return vectorEquals(objectsToCapture, other.objectsToCapture);

	return vectorEquals(objectTypes, other.objectTypes)
		&& vectorEquals(objectSubTypes, other.objectSubTypes);
}

Goals::TGoalVec CaptureObjectsBehavior::getVisitGoals(const std::vector<AIPath> & paths, const CGObjectInstance * objToVisit)
{
	Goals::TGoalVec tasks;

	tasks.reserve(paths.size());

	const AIPath * closestWay = nullptr;
	std::vector<ExecuteHeroChain *> waysToVisitObj;

	for(auto & path : paths)
	{
		tasks.push_back(sptr(Goals::Invalid()));

#if AI_TRACE_LEVEL >= 2
		logAi->trace("Path found %s", path.toString());
#endif

		if(ai->nullkiller->dangerHitMap->enemyCanKillOurHeroesAlongThePath(path))
		{
#if AI_TRACE_LEVEL >= 2
			logAi->trace("Ignore path. Target hero can be killed by enemy. Our power %lld", path.heroArmy->getArmyStrength());
#endif
			continue;
		}

		if(objToVisit && !shouldVisit(path.targetHero, objToVisit))
			continue;

		auto hero = path.targetHero;
		auto danger = path.getTotalDanger();

		if(ai->nullkiller->heroManager->getHeroRole(hero) == HeroRole::SCOUT && danger == 0 && path.exchangeCount > 1)
			continue;

		auto firstBlockedAction = path.getFirstBlockedAction();
		if(firstBlockedAction)
		{
			auto subGoal = firstBlockedAction->decompose(path.targetHero);

#if AI_TRACE_LEVEL >= 2
			logAi->trace("Decomposing special action %s returns %s", firstBlockedAction->toString(), subGoal->toString());
#endif

			if(!subGoal->invalid())
			{
				Composition composition;

				composition.addNext(ExecuteHeroChain(path, objToVisit));
				composition.addNext(subGoal);

				tasks[tasks.size() - 1] = sptr(composition);
			}

			continue;
		}

		auto isSafe = isSafeToVisit(hero, path.heroArmy, danger);

#if AI_TRACE_LEVEL >= 2
		logAi->trace(
			"It is %s to visit %s by %s with army %lld, danger %lld and army loss %lld",
			isSafe ? "safe" : "not safe",
			objToVisit ? objToVisit->getObjectName() : path.targetTile().toString(),
			hero->name,
			path.getHeroStrength(),
			danger,
			path.getTotalArmyLoss());
#endif

		if(isSafe)
		{
			auto newWay = new ExecuteHeroChain(path, objToVisit);
			TSubgoal sharedPtr;

			sharedPtr.reset(newWay);

			if(!closestWay || closestWay->movementCost() > path.movementCost())
				closestWay = &path;

			if(!ai->nullkiller->arePathHeroesLocked(path))
			{
				waysToVisitObj.push_back(newWay);
				tasks[tasks.size() - 1] = sharedPtr;
			}
		}
	}

	for(auto way : waysToVisitObj)
	{
		way->closestWayRatio
			= closestWay->movementCost() / way->getPath().movementCost();
	}

	return tasks;
}

Goals::TGoalVec CaptureObjectsBehavior::decompose() const
{
	Goals::TGoalVec tasks;

	auto captureObjects = [&](const std::vector<const CGObjectInstance*> & objs) -> void{
		if(objs.empty())
		{
			return;
		}

		logAi->debug("Scanning objects, count %d", objs.size());

		for(auto objToVisit : objs)
		{			
#if AI_TRACE_LEVEL >= 1
			logAi->trace("Checking object %s, %s", objToVisit->getObjectName(), objToVisit->visitablePos().toString());
#endif

			if(!shouldVisitObject(objToVisit))
				continue;

			const int3 pos = objToVisit->visitablePos();

			auto paths = ai->nullkiller->pathfinder->getPathInfo(pos);
			std::vector<std::shared_ptr<ExecuteHeroChain>> waysToVisitObj;
			std::shared_ptr<ExecuteHeroChain> closestWay;
					
#if AI_TRACE_LEVEL >= 1
			logAi->trace("Found %d paths", paths.size());
#endif
			vstd::concatenate(tasks, getVisitGoals(paths, objToVisit));
		}

		vstd::erase_if(tasks, [](TSubgoal task) -> bool
		{
			return task->invalid();
		});
	};

	if(specificObjects)
	{
		captureObjects(objectsToCapture);
	}
	else
	{
		captureObjects(ai->nullkiller->objectClusterizer->getNearbyObjects());

		if(tasks.empty())
			captureObjects(ai->nullkiller->objectClusterizer->getFarObjects());
	}

	return tasks;
}

bool CaptureObjectsBehavior::shouldVisitObject(const CGObjectInstance * obj) const
{
	if(objectTypes.size() && !vstd::contains(objectTypes, obj->ID.num))
	{
		return false;
	}

	if(objectSubTypes.size() && !vstd::contains(objectSubTypes, obj->subID))
	{
		return false;
	}

	return true;
}

bool CaptureObjectsBehavior::shouldVisit(HeroPtr h, const CGObjectInstance * obj)
{
	switch(obj->ID)
	{
	case Obj::TOWN:
	case Obj::HERO: //never visit our heroes at random
		return obj->tempOwner != h->tempOwner; //do not visit our towns at random
	case Obj::BORDER_GATE:
	{
		for(auto q : ai->myCb->getMyQuests())
		{
			if(q.obj == obj)
			{
				return false; // do not visit guards or gates when wandering
			}
		}
		return true; //we don't have this quest yet
	}
	case Obj::BORDERGUARD: //open borderguard if possible
		return (dynamic_cast<const CGKeys *>(obj))->wasMyColorVisited(ai->playerID);
	case Obj::SEER_HUT:
	case Obj::QUEST_GUARD:
	{
		for(auto q : ai->myCb->getMyQuests())
		{
			if(q.obj == obj)
			{
				if(q.quest->checkQuest(h.h))
					return true; //we completed the quest
				else
					return false; //we can't complete this quest
			}
		}
		return true; //we don't have this quest yet
	}
	case Obj::CREATURE_GENERATOR1:
	{
		if(obj->tempOwner != h->tempOwner)
			return true; //flag just in case

		const CGDwelling * d = dynamic_cast<const CGDwelling *>(obj);

		for(auto level : d->creatures)
		{
			for(auto c : level.second)
			{
				if(level.first
					&& h->getSlotFor(CreatureID(c)) != SlotID()
					&& cb->getResourceAmount().canAfford(c.toCreature()->cost))
				{
					return true;
				}
			}
		}

		return false;
	}
	case Obj::HILL_FORT:
	{
		for(auto slot : h->Slots())
		{
			if(slot.second->type->upgrades.size())
				return true; //TODO: check price?
		}
		return false;
	}
	case Obj::MONOLITH_ONE_WAY_ENTRANCE:
	case Obj::MONOLITH_ONE_WAY_EXIT:
	case Obj::MONOLITH_TWO_WAY:
	case Obj::WHIRLPOOL:
		return false;
	case Obj::SCHOOL_OF_MAGIC:
	case Obj::SCHOOL_OF_WAR:
	{
		if(cb->getResourceAmount(Res::GOLD) < 1000)
			return false;
		break;
	}
	case Obj::LIBRARY_OF_ENLIGHTENMENT:
		if(h->level < 12)
			return false;
		break;
	case Obj::TREE_OF_KNOWLEDGE:
	{
		if(ai->nullkiller->heroManager->getHeroRole(h) == HeroRole::SCOUT)
			return false;

		TResources myRes = cb->getResourceAmount();
		if(myRes[Res::GOLD] < 2000 || myRes[Res::GEMS] < 10)
			return false;
		break;
	}
	case Obj::MAGIC_WELL:
		return h->mana < h->manaLimit();
	case Obj::PRISON:
		return ai->myCb->getHeroesInfo().size() < VLC->modh->settings.MAX_HEROES_ON_MAP_PER_PLAYER;
	case Obj::TAVERN:
	{
		//TODO: make AI actually recruit heroes
		//TODO: only on request
		if(ai->myCb->getHeroesInfo().size() >= VLC->modh->settings.MAX_HEROES_ON_MAP_PER_PLAYER)
			return false;
		else if(cb->getResourceAmount(Res::GOLD) < GameConstants::HERO_GOLD_COST)
			return false;
		break;
	}
	case Obj::BOAT:
		return false;
		//Boats are handled by pathfinder
	case Obj::EYE_OF_MAGI:
		return false; //this object is useless to visit, but could be visited indefinitely
	}

	if(obj->wasVisited(*h)) //it must pointer to hero instance, heroPtr calls function wasVisited(ui8 player);
		return false;

	return true;
}