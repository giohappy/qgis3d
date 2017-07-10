#include "demterraingenerator.h"

#include "map3d.h"
#include "quadtree.h"
#include "demterraintilegeometry.h"
#include "maptexturegenerator.h"

#include <Qt3DRender/QGeometryRenderer>

#include "qgsrasterlayer.h"

static QByteArray _temporaryHeightMap(int res)
{
  QByteArray heightMap;
  int count = res * res;
  heightMap.resize(count * sizeof(float));
  float* bits = (float*) heightMap.data();
  for (int i = 0; i < count; ++i)
    bits[i] = 0;
  return heightMap;
}

static void _heightMapMinMax(const QByteArray& heightMap, float& zMin, float& zMax)
{
  const float* zBits = (const float*) heightMap.constData();
  int zCount = heightMap.count() / sizeof(float);
  zMin = zBits[0];
  zMax = zBits[0];
  for (int i = 0; i < zCount; ++i)
  {
    float z = zBits[i];
    zMin = qMin(zMin, z);
    zMax = qMax(zMax, z);
  }
}

DemTerrainTile::DemTerrainTile(QuadTreeNode *node, const Map3D& map, Qt3DCore::QNode *parent)
  : TerrainTileEntity(node, map, parent)
{
  DemTerrainGenerator* generator = static_cast<DemTerrainGenerator*>(map.terrainGenerator.get());

  // generate a temporary heightmap
  // TODO: use upsampled heightmap from parent
  QByteArray heightMap = _temporaryHeightMap(generator->demTerrainSize);

  // async request for heightmap
  jobId = generator->tGen->render(node->x, node->y, node->level);
  connect(generator->tGen.get(), &DemHeightMapGenerator::heightMapReady, this, &DemTerrainTile::onHeightMapReady);

  float zMin, zMax;
  _heightMapMinMax(heightMap, zMin, zMax);

  Qt3DRender::QGeometryRenderer* mesh = new Qt3DRender::QGeometryRenderer;
  geometry = new DemTerrainTileGeometry(generator->tGen->resolution(), heightMap, mesh);
  mesh->setGeometry(geometry);
  addComponent(mesh);  // takes ownership if the component has no parent

  QgsRectangle extent = node->extent;
  double x0 = extent.xMinimum() - map.originX;
  double y0 = extent.yMinimum() - map.originY;
  double side = extent.width();
  double half = side/2;

  transform->setScale3D(QVector3D(side, map.zExaggeration, side));
  transform->setTranslation(QVector3D(x0 + half,0, - (y0 + half)));

  bbox = AABB(x0, zMin*map.zExaggeration, -y0, x0 + side, zMax*map.zExaggeration, -(y0 + side));
  epsilon = side / map.tileTextureSize;  // use texel size as the error
}

void DemTerrainTile::onHeightMapReady(int jobId, const QByteArray &heightMap)
{
  if (jobId == this->jobId)
  {
    geometry->setHeightMap(heightMap);

    // also update our bbox!
    float zMin, zMax;
    _heightMapMinMax(heightMap, zMin, zMax);
    bbox.yMin = zMin*m_map.zExaggeration;
    bbox.yMax = zMax*m_map.zExaggeration;
  }
}


// ---------------


DemTerrainGenerator::DemTerrainGenerator(QgsRasterLayer *dem, int terrainSize)
{
  demLayer = dem;
  demTerrainSize = terrainSize;
  terrainTilingScheme = TilingScheme(dem->extent(), dem->crs());
  tGen.reset(new DemHeightMapGenerator(demLayer, terrainTilingScheme, demTerrainSize));
}

TerrainGenerator::Type DemTerrainGenerator::type() const
{
  return TerrainGenerator::Dem;
}

QgsRectangle DemTerrainGenerator::extent() const
{
  return terrainTilingScheme.tileToExtent(0, 0, 0);
}

TerrainTileEntity *DemTerrainGenerator::createTile(QuadTreeNode *n, const Map3D &map, Qt3DCore::QNode *parent) const
{
  return new DemTerrainTile(n, map, parent);
}



// ---------------------

#include <qgsrasterlayer.h>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

DemHeightMapGenerator::DemHeightMapGenerator(QgsRasterLayer *dtm, const TilingScheme &tilingScheme, int resolution)
  : dtm(dtm)
  , tilingScheme(tilingScheme)
  , res(resolution)
  , lastJobId(0)
{
}

DemHeightMapGenerator::~DemHeightMapGenerator()
{
}

static QByteArray _readDtmData(QgsRasterDataProvider* provider, const QgsRectangle& extent, int res)
{
  // TODO: use feedback object? (but GDAL currently does not support cancellation anyway)
  QgsRasterBlock* block = provider->block(1, extent, res, res);
  delete provider;

  QByteArray data;
  if (block)
  {
    block->convert(Qgis::Float32);   // currently we expect just floats
    data = block->data();
    data.detach();  // this should make a deep copy
    delete block;
  }
  return data;
}

int DemHeightMapGenerator::render(int x, int y, int z)
{
  // extend the rect by half-pixel on each side? to get the values in "corners"
  QgsRectangle extent = tilingScheme.tileToExtent(x, y, z);
  float mapUnitsPerPixel = extent.width() / res;
  extent.grow( mapUnitsPerPixel / 2);
  // but make sure not to go beyond the full extent (returns invalid values)
  QgsRectangle fullExtent = tilingScheme.tileToExtent(0, 0, 0);
  extent = extent.intersect(&fullExtent);

  JobData jd;
  jd.jobId = ++lastJobId;
  jd.extent = extent;
  // make a clone of the data provider so it is safe to use in worker thread
  jd.future = QtConcurrent::run(_readDtmData, (QgsRasterDataProvider*)dtm->dataProvider()->clone(), extent, res);

  QFutureWatcher<QByteArray>* fw = new QFutureWatcher<QByteArray>;
  fw->setFuture(jd.future);
  connect(fw, &QFutureWatcher<QByteArray>::finished, this, &DemHeightMapGenerator::onFutureFinished);

  jobs.insert(fw, jd);
  qDebug() << "[TT] added job: " << jd.jobId << " " << x << "|" << y << "|" << z << "  .... in queue: " << jobs.count();

  return jd.jobId;
}

void DemHeightMapGenerator::onFutureFinished()
{
  QFutureWatcher<QByteArray>* fw = static_cast<QFutureWatcher<QByteArray>*>(sender());
  Q_ASSERT(fw);
  Q_ASSERT(jobs.contains(fw));
  JobData jobData = jobs.value(fw);

  jobs.remove(fw);
  fw->deleteLater();
  qDebug() << "[TT] finished job " << jobData.jobId << "  ... in queue: " << jobs.count();

  QByteArray data = jobData.future.result();
  emit heightMapReady(jobData.jobId, data);
}
