/*
 * Copyright (C) 2010-2011 Dmitry Marakasov
 *
 * This file is part of glosm.
 *
 * glosm is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glosm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glosm.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glosm/TileManager.hh>

#include <glosm/Viewer.hh>
#include <glosm/Geometry.hh>
#include <glosm/GeometryDatasource.hh>
#include <glosm/GeometryOperations.hh>
#include <glosm/Tile.hh>
#include <glosm/Exception.hh>

#include <glosm/util/gl.h>

TileManager::TileManager(const Projection projection): projection_(projection), loading_(-1, -1, -1) {
	generation_ = 0;
	thread_die_flag_ = false;

	int errn;

	if ((errn = pthread_mutex_init(&tiles_mutex_, 0)) != 0)
		throw SystemError(errn) << "pthread_mutex_init failed";

	if ((errn = pthread_mutex_init(&queue_mutex_, 0)) != 0) {
		pthread_mutex_destroy(&tiles_mutex_);
		throw SystemError(errn) << "pthread_mutex_init failed";
	}

	if ((errn = pthread_cond_init(&queue_cond_, 0)) != 0) {
		pthread_mutex_destroy(&tiles_mutex_);
		pthread_mutex_destroy(&queue_mutex_);
		throw SystemError(errn) << "pthread_cond_init failed";
	}

	if ((errn = pthread_create(&loading_thread_, NULL, LoadingThreadFuncWrapper, (void*)this)) != 0) {
		pthread_mutex_destroy(&tiles_mutex_);
		pthread_mutex_destroy(&queue_mutex_);
		pthread_cond_destroy(&queue_cond_);
		throw SystemError(errn) << "pthread_create failed";
	}

	lowres_level_ = 8;
	hires_level_ = 13;
	lowres_range_ = 1000000.0f;
	hires_range_ = 10000.0f;
}


TileManager::~TileManager() {
	thread_die_flag_ = true;
	pthread_cond_signal(&queue_cond_);

	/* TODO: check exit code */
	pthread_join(loading_thread_, NULL);

	pthread_cond_destroy(&queue_cond_);
	pthread_mutex_destroy(&queue_mutex_);
	pthread_mutex_destroy(&tiles_mutex_);

	RecDestroyTiles(&root_);
}

/*
 * recursive quadtree processing
 */

void TileManager::RecLoadTiles(RecLoadTilesInfo& info, QuadNode** pnode, int level, int x, int y) {
	QuadNode* node;
	float thisdist;

	if (*pnode == NULL) {
		/* no node; check if it's in view and if yes, create it */
		BBoxi bbox = BBoxi::ForGeoTile(level, x, y);
		thisdist = ApproxDistanceSquare(bbox, info.viewer_pos.Flattened());
		if (thisdist > hires_range_ * hires_range_)
			return;
		node = *pnode = new QuadNode;
		node->bbox = bbox;
	} else {
		/* node exists, visit it if it's in view */
		node = *pnode;
		thisdist = ApproxDistanceSquare(node->bbox, info.viewer_pos.Flattened());
		if (thisdist > hires_range_ * hires_range_)
			return;
	}
	/* range check passed and node exists */

	node->generation = generation_;

	if (level == hires_level_) {
		if (node->tile)
			return; /* tile already loaded */

		if (loading_ != TileId(level, x, y)) {
			if (queue_.empty()) {
				info.closest_distance = thisdist;
				queue_.push_front(TileTask(TileId(level, x, y), node->bbox));
				info.queue_size++;
			} else {
				if (thisdist < info.closest_distance) {
					/* this tile is closer than the best we have in the queue - push to front so it's downloaded faster */
					queue_.push_front(TileTask(TileId(level, x, y), node->bbox));
					info.closest_distance = thisdist;
					info.queue_size++;
				} else if (info.queue_size < 100) {
					/* push into the back of the queue, but not if queue is too long */
					queue_.push_back(TileTask(TileId(level, x, y), node->bbox));
					info.queue_size++;
				}
			}
		}

		/* no more recursion is needed */
		return;
	}

	/* recurse */
	RecLoadTiles(info, node->childs, level+1, x * 2, y * 2);
	RecLoadTiles(info, node->childs + 1, level+1, x * 2 + 1, y * 2);
	RecLoadTiles(info, node->childs + 2, level+1, x * 2, y * 2 + 1);
	RecLoadTiles(info, node->childs + 3, level+1, x * 2 + 1, y * 2 + 1);

	return;
}

void TileManager::RecPlaceTile(QuadNode* node, Tile* tile, int level, int x, int y) {
	if (node == NULL) {
		/* part of quadtree was garbage collected -> tile
		 * is no longer needed and should just be dropped */
		delete tile;
		return;
	}

	if (level == 0) {
		if (node->tile != NULL) {
			/* tile already loaded for some reason (sync loading?)
			 * -> drop copy */
			delete tile;
			return;
		}
		node->tile = tile;
	} else {
		int mask = 1 << (level-1);
		int nchild = (!!(y & mask) << 1) | !!(x & mask);
		RecPlaceTile(node->childs[nchild], tile, level-1, x, y);
	}
}

void TileManager::RecDestroyTiles(QuadNode* node) {
	if (!node)
		return;

	if (node->tile)
		delete node->tile;

	for (int i = 0; i < 4; ++i) {
		RecDestroyTiles(node->childs[i]);
		delete node->childs[i];
	}
}

void TileManager::RecGarbageCollectTiles(QuadNode* node) {
	/* simplest garbage collection that drops all inactive
	 * tiles. This should become much more clever */
	for (int i = 0; i < 4; ++i) {
		if (node->childs[i] == NULL)
			continue;

		if (node->childs[i]->generation != generation_) {
			RecDestroyTiles(node->childs[i]);
			delete node->childs[i];
			node->childs[i] = NULL;
		} else {
			RecGarbageCollectTiles(node->childs[i]);
		}
	}
}

void TileManager::RecRenderTiles(QuadNode* node, const Viewer& viewer) {
	if (!node || node->generation != generation_)
		return;

	RecRenderTiles(node->childs[0], viewer);
	RecRenderTiles(node->childs[1], viewer);
	RecRenderTiles(node->childs[2], viewer);
	RecRenderTiles(node->childs[3], viewer);

	if (node->tile) {
		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();

		/* prepare modelview matrix for the tile: position
		 * it in the right place given that viewer is always
		 * at (0, 0, 0) */
		Vector3f offset = projection_.Project(node->tile->GetReference(), Vector2i(viewer.GetPos(projection_))) +
				projection_.Project(Vector2i(viewer.GetPos(projection_)), viewer.GetPos(projection_));

		glTranslatef(offset.x, offset.y, offset.z);

		/* same for rotation */
		Vector3i ref = node->tile->GetReference();
		Vector3i pos = viewer.GetPos(projection_);

		/* normal at tile's reference point */
		Vector3d refnormal = (
				(Vector3d)projection_.Project(Vector3i(ref.x, ref.y, std::numeric_limits<osmint_t>::max()), pos) -
				(Vector3d)projection_.Project(Vector3i(ref.x, ref.y, 0), pos)
			).Normalized();

		/* normal at reference point projected to equator */
		Vector3d refeqnormal = (
				(Vector3d)projection_.Project(Vector3i(ref.x, 0, std::numeric_limits<osmint_t>::max()), pos) -
				(Vector3d)projection_.Project(Vector3i(ref.x, 0, 0), pos)
			).Normalized();

		/* normal at north pole */
		Vector3d polenormal = (
				(Vector3d)projection_.Project(Vector3i(ref.x, 900000000, std::numeric_limits<osmint_t>::max()), pos) -
				(Vector3d)projection_.Project(Vector3i(ref.x, 900000000, 0), pos)
			).Normalized();

		/* XXX: IsValid() check basically detects
		 * MercatorProjection and does no rotation for it.
		 * While is's ok for now, this may need more generic
		 * approach in future */
		if (polenormal.IsValid()) {
			Vector3d side = refnormal.CrossProduct(polenormal).Normalized();

			glRotatef((double)((osmlong_t)ref.y - (osmlong_t)pos.y) / 10000000.0, side.x, side.y, side.z);
			glRotatef((double)((osmlong_t)ref.x - (osmlong_t)pos.x) / 10000000.0, polenormal.x, polenormal.y, polenormal.z);
		}

		node->tile->Render();

		glMatrixMode(GL_MODELVIEW);
		glPopMatrix();
	}
}

/*
 * loading queue - related
 */

void TileManager::LoadingThreadFunc() {
	pthread_mutex_lock(&queue_mutex_);
	while (!thread_die_flag_) {
		/* found nothing, sleep */
		if (queue_.empty()) {
			pthread_cond_wait(&queue_cond_, &queue_mutex_);
			continue;
		}

		/* take a task from the queue */
		TileTask task = queue_.front();
		queue_.pop_front();

		/* mark it as loading */
		loading_ = task.id;

		pthread_mutex_unlock(&queue_mutex_);

		/* load tile */
		Tile* tile = SpawnTile(task.bbox);

		pthread_mutex_lock(&tiles_mutex_);
		RecPlaceTile(&root_, tile, task.id.level, task.id.x, task.id.y);

		pthread_mutex_unlock(&tiles_mutex_);

		/* The following happens:
		 * - main thread finishes Render and unlocks tiles_mutex_
		 * - this thread wakes up and loads MANY tiles without main
		 *   thread able to run
		 * - main thread finally runs after ~0.1 sec delay, which
		 *   is noticeable lag in realtime renderer
		 *
		 * Nut sure yet how to fix ot properly, but this sched_yield
		 * works for now.
		 */
		sched_yield();

		pthread_mutex_lock(&queue_mutex_);
		loading_ = TileId(-1, -1, -1);
	}
	pthread_mutex_unlock(&queue_mutex_);
}

void* TileManager::LoadingThreadFuncWrapper(void* arg) {
	static_cast<TileManager*>(arg)->LoadingThreadFunc();
	return NULL;
}

/*
 * protected interface
 */

void TileManager::Render(const Viewer& viewer) {
	pthread_mutex_lock(&tiles_mutex_);
	RecRenderTiles(&root_, viewer);
	pthread_mutex_unlock(&tiles_mutex_);
}

/*
 * public interface
 */

void TileManager::LoadArea(const BBoxi& bbox, int flags) {
}

void TileManager::LoadLocality(const Viewer& viewer, int flags) {
	if (!(flags & SYNC)) {
		pthread_mutex_lock(&queue_mutex_);
		queue_.clear();
	}

	pthread_mutex_lock(&tiles_mutex_);

	RecLoadTilesInfo info(viewer, flags);
	info.viewer_pos = viewer.GetPos(projection_);

	QuadNode* root = &root_;
	RecLoadTiles(info, &root);

	pthread_mutex_unlock(&tiles_mutex_);

	if (!(flags & SYNC)) {
		pthread_mutex_unlock(&queue_mutex_);

		if (!queue_.empty())
			pthread_cond_signal(&queue_cond_);
	}
}

void TileManager::GarbageCollect() {
	pthread_mutex_lock(&tiles_mutex_);
	RecGarbageCollectTiles(&root_);
	generation_++;
	pthread_mutex_unlock(&tiles_mutex_);
}
