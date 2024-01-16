#pragma once

#include "framework/zstring.h"
#include "hw_levelmesh.h"
#include "level/doomdata.h"

struct FLevel;
class FWadWriter;

class DoomLevelMesh : public LevelMesh
{
public:
	DoomLevelMesh(FLevel& doomMap);

	int AddSurfaceLights(const LevelMeshSurface* surface, LevelMeshLight* list, int listMaxSize) override;

	void BeginFrame(FLevel& doomMap);
	bool TraceSky(const FVector3& start, FVector3 direction, float dist);
	void DumpMesh(const FString& objFilename, const FString& mtlFilename) const;
	void AddLightmapLump(FLevel& doomMap, FWadWriter& out);

	void BuildSectorGroups(const FLevel& doomMap);

	TArray<int> sectorGroup; // index is sector, value is sectorGroup
	TArray<int> sectorPortals[2]; // index is sector+plane, value is index into the portal list
	TArray<int> linePortals; // index is linedef, value is index into the portal list

private:
	void CreatePortals(FLevel& doomMap);
	void BuildLightLists(FLevel& doomMap);
	void PropagateLight(FLevel& doomMap, ThingLight* light, int recursiveDepth = 0);
};

#if 0

class DoomLevelMesh : public LevelMesh
{
public:
	DoomLevelMesh(FLevel& level, int samples, int lmdims);

	int AddSurfaceLights(const LevelMeshSurface* surface, LevelMeshLight* list, int listMaxSize) override;
	void DumpMesh(const FString& objFilename, const FString& mtlFilename) const;
	void AddLightmapLump(FLevel& doomMap, FWadWriter& out);

private:
	void BuildLightLists(FLevel& doomMap);
	void PropagateLight(FLevel& doomMap, ThingLight* light, int recursiveDepth = 0);

	// Portal to portals[] index
	//std::map<Portal, int, IdenticalPortalComparator> portalCache;

	// Portal lights
	//std::vector<std::unique_ptr<ThingLight>> portalLights;
	//std::set<Portal, RecursivePortalComparator> touchedPortals;
};

enum DoomLevelMeshSurfaceType
{
	ST_UNKNOWN,
	ST_MIDDLESIDE,
	ST_UPPERSIDE,
	ST_LOWERSIDE,
	ST_CEILING,
	ST_FLOOR
};

struct DoomLevelMeshSurface : public LevelMeshSurface
{
	DoomLevelMeshSurfaceType Type = ST_UNKNOWN;
	int TypeIndex = 0;

	MapSubsectorEx* Subsector = nullptr;
	IntSideDef* Side = nullptr;
	IntSector* ControlSector = nullptr;

	float* TexCoords = nullptr;

	std::vector<ThingLight*> LightList;
};

class DoomLevelSubmesh : public LevelSubmesh
{
public:
	void CreateStatic(FLevel& doomMap);

	LevelMeshSurface* GetSurface(int index) override { return &Surfaces[index]; }
	unsigned int GetSurfaceIndex(const LevelMeshSurface* surface) const override { return (unsigned int)(ptrdiff_t)(static_cast<const DoomLevelMeshSurface*>(surface) - Surfaces.Data()); }
	int GetSurfaceCount() override { return Surfaces.Size(); }

	void DumpMesh(const FString& objFilename, const FString& mtlFilename) const;

	// Used by Maploader
	void BindLightmapSurfacesToGeometry(FLevel& doomMap);
	void PackLightmapAtlas(int lightmapStartIndex);
	void CreatePortals(FLevel& doomMap);

	TArray<DoomLevelMeshSurface> Surfaces;
	TArray<FVector2> LightmapUvs;
	TArray<int> sectorGroup; // index is sector, value is sectorGroup

private:
	void BuildSectorGroups(const FLevel& doomMap);

	void CreateSubsectorSurfaces(FLevel& doomMap);
	void CreateCeilingSurface(FLevel& doomMap, MapSubsectorEx* sub, IntSector* sector, IntSector* controlSector, int typeIndex);
	void CreateFloorSurface(FLevel& doomMap, MapSubsectorEx* sub, IntSector* sector, IntSector* controlSector, int typeIndex);
	void CreateSideSurfaces(FLevel& doomMap, IntSideDef* side);
	void CreateLinePortalSurface(FLevel& doomMap, IntSideDef* side);
	void CreateLineHorizonSurface(FLevel& doomMap, IntSideDef* side);
	void CreateFrontWallSurface(FLevel& doomMap, IntSideDef* side);
	void CreateTopWallSurface(FLevel& doomMap, IntSideDef* side);
	void CreateMidWallSurface(FLevel& doomMap, IntSideDef* side);
	void CreateBottomWallSurface(FLevel& doomMap, IntSideDef* side);
	void Create3DFloorWallSurfaces(FLevel& doomMap, IntSideDef* side);
	void SetSideTextureUVs(DoomLevelMeshSurface& surface, IntSideDef* side, WallPart texpart, float v1TopZ, float v1BottomZ, float v2TopZ, float v2BottomZ);

	void SetupLightmapUvs(FLevel& doomMap);

	void CreateIndexes();

	static bool IsTopSideSky(IntSector* frontsector, IntSector* backsector, IntSideDef* side);
	static bool IsTopSideVisible(IntSideDef* side);
	static bool IsBottomSideVisible(IntSideDef* side);
	static bool IsSkySector(IntSector* sector, SecPlaneType plane);

	static FVector4 ToPlane(const FVector3& pt1, const FVector3& pt2, const FVector3& pt3)
	{
		FVector3 n = ((pt2 - pt1) ^ (pt3 - pt2)).Unit();
		float d = pt1 | n;
		return FVector4(n.X, n.Y, n.Z, d);
	}

	static FVector4 ToPlane(const FVector3& pt1, const FVector3& pt2, const FVector3& pt3, const FVector3& pt4)
	{
		if (pt1.ApproximatelyEquals(pt3))
		{
			return ToPlane(pt1, pt2, pt4);
		}
		else if (pt1.ApproximatelyEquals(pt2) || pt2.ApproximatelyEquals(pt3))
		{
			return ToPlane(pt1, pt3, pt4);
		}

		return ToPlane(pt1, pt2, pt3);
	}

	// Lightmapper

	enum PlaneAxis
	{
		AXIS_YZ = 0,
		AXIS_XZ,
		AXIS_XY
	};

	static PlaneAxis BestAxis(const FVector4& p);
	BBox GetBoundsFromSurface(const LevelMeshSurface& surface) const;

	inline int AllocUvs(int amount) { return LightmapUvs.Reserve(amount); }

	void BuildSurfaceParams(int lightMapTextureWidth, int lightMapTextureHeight, LevelMeshSurface& surface);

	static bool IsDegenerate(const FVector3& v0, const FVector3& v1, const FVector3& v2);

	static FVector2 ToFVector2(const DVector2& v) { return FVector2((float)v.X, (float)v.Y); }
	static FVector3 ToFVector3(const DVector3& v) { return FVector3((float)v.X, (float)v.Y, (float)v.Z); }
	static FVector4 ToFVector4(const DVector4& v) { return FVector4((float)v.X, (float)v.Y, (float)v.Z, (float)v.W); }
};

#endif
