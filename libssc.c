#include <petsc/private/pcimpl.h>     /*I "petscpc.h" I*/
#include <petsc.h>
#include <../src/sys/utils/hash.h>
#include <petscsf.h>
#include <libssc.h>

typedef struct {
    DM              dm;         /* DMPlex object describing mesh
                                 * topology (need not be the same as
                                 * PC's DM) */
    PetscSF         globalToLocal; /* Scatter from assembled Vec to
                                    * local Vec (concatenated patches) */
    PetscSF         defaultSF;
    PetscSection    dofSection;
    PetscSection    cellCounts;
    PetscSection    cellNumbering; /* Numbering of cells in DM */
    PetscSection    localToPatch;   /* Indices to extract from local to
                                     * patch vectors */
    PetscSection    bcCounts;
    IS              cells;
    IS              dofs;
    IS              bcs;

    MPI_Datatype    data_type;
    PetscBool       free_type;

    PetscBool       save_operators; /* Save all operators (or create/destroy one at a time?) */
    PetscInt        npatch;     /* Number of patches */
    PetscInt        numBcs;     /* Number of BC nodes */
    PetscInt        bs;            /* block size (can come from global
                                    * operators?) */
    PetscInt        nodesPerCell;
    const PetscInt *bcNodes;    /* BC nodes */
    const PetscInt *cellNodeMap; /* Map from cells to nodes */

    KSP            *ksp;        /* Solvers for each patch */
    Vec             localX, localY; /* Work vectors for globaltolocal */
    Vec            *patchX, *patchY; /* Work vectors for patches */
    Mat            *mat;        /* Operators */
    MatType         sub_mat_type;
    PetscErrorCode (*usercomputeop)(PC, Mat, PetscInt, const PetscInt *, PetscInt, const PetscInt *, void *);
    void           *usercomputectx;
} PC_PATCH;

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetDMPlex"
PETSC_EXTERN PetscErrorCode PCPatchSetDMPlex(PC pc, DM dm)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->dm = dm;
    ierr = PetscObjectReference((PetscObject)dm); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetSaveOperators"
PETSC_EXTERN PetscErrorCode PCPatchSetSaveOperators(PC pc, PetscBool flg)
{
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->save_operators = flg;
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetDefaultSF"
PETSC_EXTERN PetscErrorCode PCPatchSetDefaultSF(PC pc, PetscSF sf)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->defaultSF = sf;
    ierr = PetscObjectReference((PetscObject)sf); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetCellNumbering"
PETSC_EXTERN PetscErrorCode PCPatchSetCellNumbering(PC pc, PetscSection cellNumbering)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->cellNumbering = cellNumbering;
    ierr = PetscObjectReference((PetscObject)cellNumbering); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}


#undef __FUNCT__
#define __FUNCT__ "PCPatchSetDiscretisationInfo"
PETSC_EXTERN PetscErrorCode PCPatchSetDiscretisationInfo(PC pc, PetscSection dofSection,
                                                         PetscInt bs,
                                                         PetscInt nodesPerCell,
                                                         const PetscInt *cellNodeMap,
                                                         PetscInt numBcs,
                                                         const PetscInt *bcNodes)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->dofSection = dofSection;
    ierr = PetscObjectReference((PetscObject)dofSection); CHKERRQ(ierr);
    patch->bs = bs;
    patch->nodesPerCell = nodesPerCell;
    /* Not freed here. */
    patch->cellNodeMap = cellNodeMap;
    patch->numBcs = numBcs;
    patch->bcNodes = bcNodes;
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetSubMatType"
PETSC_EXTERN PetscErrorCode PCPatchSetSubMatType(PC pc, MatType sub_mat_type)
{
    PC_PATCH *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;
    patch->sub_mat_type = sub_mat_type;
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetComputeOperator"
PETSC_EXTERN PetscErrorCode PCPatchSetComputeOperator(PC pc, PetscErrorCode (*func)(PC, Mat, PetscInt,
                                                                                    const PetscInt *,
                                                                                    PetscInt,
                                                                                    const PetscInt *,
                                                                                    void *),
                                                      void *ctx)
{
    PC_PATCH *patch = (PC_PATCH *)pc->data;

    PetscFunctionBegin;
    patch->usercomputeop = func;
    patch->usercomputectx = ctx;

    PetscFunctionReturn(0);
}
#undef __FUNCT__
#define __FUNCT__ "PCPatchCreateCellPatches"
/*
 * PCPatchCreateCellPatches - create patches of cells around vertices in the mesh.
 *
 * Input Parameters:
 * + dm - The DMPlex object defining the mesh
 *
 * Output Parameters:
 * + cellCounts - Section with counts of cells around each vertex
 * - cells - IS of the cell point indices of cells in each patch
 */
static PetscErrorCode PCPatchCreateCellPatches(DM dm,
                                               PetscSection *cellCounts,
                                               IS *cells)
{
    PetscErrorCode  ierr;
    DMLabel         core, non_core;
    PetscInt        pStart, pEnd, vStart, vEnd, cStart, cEnd;
    PetscBool       flg1, flg2;
    PetscInt        closureSize;
    PetscInt       *closure    = NULL;
    PetscInt       *cellsArray = NULL;
    PetscInt        numCells;

    PetscFunctionBegin;

    ierr = DMPlexGetChart(dm, &pStart, &pEnd); CHKERRQ(ierr);
    ierr = DMPlexGetDepthStratum(dm, 0, &vStart, &vEnd); CHKERRQ(ierr);
    ierr = DMPlexGetHeightStratum(dm, 0, &cStart, &cEnd); CHKERRQ(ierr);

    /* These labels mark the owned points.  We only create patches
     * around vertices that this process owns. */
    ierr = DMGetLabel(dm, "op2_core", &core); CHKERRQ(ierr);
    ierr = DMGetLabel(dm, "op2_non_core", &non_core); CHKERRQ(ierr);

    ierr = DMLabelCreateIndex(core, pStart, pEnd); CHKERRQ(ierr);
    ierr = DMLabelCreateIndex(non_core, pStart, pEnd); CHKERRQ(ierr);

    ierr = PetscSectionCreate(PETSC_COMM_SELF, cellCounts); CHKERRQ(ierr);
    ierr = PetscSectionSetChart(*cellCounts, vStart, vEnd); CHKERRQ(ierr);

    /* Count cells surrounding each vertex */
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        ierr = DMLabelHasPoint(core, v, &flg1); CHKERRQ(ierr);
        ierr = DMLabelHasPoint(non_core, v, &flg2); CHKERRQ(ierr);
        /* Not an owned vertex, don't make a cell patch. */
        if (!(flg1 || flg2)) {
            continue;
        }
        ierr = DMPlexGetTransitiveClosure(dm, v, PETSC_FALSE, &closureSize, &closure); CHKERRQ(ierr);
        for ( PetscInt ci = 0; ci < closureSize; ci++ ) {
            const PetscInt c = closure[2*ci];
            if (cStart <= c && c < cEnd) {
                ierr = PetscSectionAddDof(*cellCounts, v, 1); CHKERRQ(ierr);
            }
        }
    }
    ierr = DMLabelDestroyIndex(core); CHKERRQ(ierr);
    ierr = DMLabelDestroyIndex(non_core); CHKERRQ(ierr);

    ierr = PetscSectionSetUp(*cellCounts); CHKERRQ(ierr);
    ierr = PetscSectionGetStorageSize(*cellCounts, &numCells); CHKERRQ(ierr);
    ierr = PetscMalloc1(numCells, &cellsArray); CHKERRQ(ierr);

    /* Now that we know how much space we need, run through again and
     * actually remember the cells. */
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        PetscInt ndof, off;
        ierr = PetscSectionGetDof(*cellCounts, v, &ndof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(*cellCounts, v, &off); CHKERRQ(ierr);
        if ( ndof <= 0 ) {
            continue;
        }
        ierr = DMPlexGetTransitiveClosure(dm, v, PETSC_FALSE, &closureSize, &closure); CHKERRQ(ierr);
        ndof = 0;
        for ( PetscInt ci = 0; ci < closureSize; ci++ ) {
            const PetscInt c = closure[2*ci];
            if (cStart <= c && c < cEnd) {
                cellsArray[ndof + off] = c;
                ndof++;
            }
        }
    }
    ierr = DMPlexRestoreTransitiveClosure(dm, 0, PETSC_FALSE, &closureSize, &closure); CHKERRQ(ierr);

    ierr = ISCreateGeneral(PETSC_COMM_SELF, numCells, cellsArray, PETSC_OWN_POINTER, cells); CHKERRQ(ierr);

    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchCreateCellPatchFacets"
/*
 * PCPatchCreateCellPatchFacets - Build the boundary facets for each cell patch
 *
 * Input Parameters:
 * + dm - The DMPlex object defining the mesh
 * . cellCounts - Section with counts of cells around each vertex
 * - cells - IS of the cell point indices of cells in each patch
 *
 * Output Parameters:
 * + facetCounts - Section with counts of boundary facets for each cell patch
 * - facets - IS of the boundary facet point indices for each cell patch.
 *
 * Note:
 *  The output facets do not include those facets that are the
 *  boundary of the domain, they are treated separately.
 */
static PetscErrorCode PCPatchCreateCellPatchFacets(DM dm,
                                                   PetscSection cellCounts,
                                                   IS cells,
                                                   PetscSection *facetCounts,
                                                   IS *facets)
{
    PetscErrorCode  ierr;
    PetscInt        vStart, vEnd, fStart, fEnd;
    DMLabel         facetLabel;
    PetscBool       flg;
    PetscInt        totalFacets, facetIndex;
    const PetscInt *cellFacets  = NULL;
    const PetscInt *facetCells  = NULL;
    PetscInt       *facetsArray = NULL;
    const PetscInt *cellsArray  = NULL;
    PetscHashI      ht;

    PetscFunctionBegin;

    /* This label marks facets exterior to the domain, which we don't
     * treat here. */
    ierr = DMGetLabel(dm, "exterior_facets", &facetLabel); CHKERRQ(ierr);

    ierr = DMPlexGetDepthStratum(dm, 0, &vStart, &vEnd); CHKERRQ(ierr);
    ierr = DMPlexGetHeightStratum(dm, 1, &fStart, &fEnd); CHKERRQ(ierr);
    ierr = DMLabelCreateIndex(facetLabel, fStart, fEnd); CHKERRQ(ierr);

    ierr = PetscSectionCreate(PETSC_COMM_SELF, facetCounts); CHKERRQ(ierr);
    ierr = PetscSectionSetChart(*facetCounts, vStart, vEnd); CHKERRQ(ierr);

    /* OK, so now we know the cells in each patch, and need to
     * determine the facets that live on the boundary of each patch.
     * We will apply homogeneous dirichlet bcs to the dofs on the
     * boundary.  The exception is for facets that are exterior to
     * the whole domain (where the normal bcs are applied). */

    /* Used to keep track of the cells in the patch. */
    PetscHashICreate(ht);

    /* Guess at number of facets: each cell contributes one facet to
     * the boundary facets.  Hopefully we will only realloc a little
     * bit.  This is a good guess for simplices, but not as good for
     * quads. */
    ierr = ISGetSize(cells, &totalFacets); CHKERRQ(ierr);
    ierr = PetscMalloc1(totalFacets, &facetsArray); CHKERRQ(ierr);
    ierr = ISGetIndices(cells, &cellsArray); CHKERRQ(ierr);
    facetIndex = 0;
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        PetscInt ndof, off;
        ierr = PetscSectionGetDof(cellCounts, v, &ndof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(cellCounts, v, &off); CHKERRQ(ierr);
        if ( ndof <= 0 ) {
            /* No cells around this vertex. */
            continue;
        }
        PetscHashIClear(ht);
        for ( PetscInt ci = off; ci < ndof + off; ci++ ) {
            const PetscInt c = cellsArray[ci];
            PetscHashIAdd(ht, c, 0);
        }
        for ( PetscInt ci = off; ci < ndof + off; ci++ ) {
            const PetscInt c = cellsArray[ci];
            PetscInt       numFacets, numCells;
            /* Facets of each cell */
            ierr = DMPlexGetCone(dm, c, &cellFacets); CHKERRQ(ierr);
            ierr = DMPlexGetConeSize(dm, c, &numFacets); CHKERRQ(ierr);
            for ( PetscInt j = 0; j < numFacets; j++ ) {
                const PetscInt f = cellFacets[j];
                ierr = DMLabelHasPoint(facetLabel, f, &flg); CHKERRQ(ierr);
                if (flg) {
                    /* Facet is on domain boundary, don't select it */
                    continue;
                }
                /* Cells in the support of the facet */
                ierr = DMPlexGetSupport(dm, f, &facetCells); CHKERRQ(ierr);
                ierr = DMPlexGetSupportSize(dm, f, &numCells); CHKERRQ(ierr);
                if (numCells == 1) {
                    /* This facet is on a process boundary, therefore
                     * also a patch boundary */
                    ierr = PetscSectionAddDof(*facetCounts, v, 1); CHKERRQ(ierr);
                    goto addFacet;
                } else {
                    for ( PetscInt k = 0; k < numCells; k++ ) {
                        PetscHashIHasKey(ht, facetCells[k], flg);
                        if (!flg) {
                            /* Facet's cell is not in the patch, so
                             * it's on the patch boundary. */
                            ierr = PetscSectionAddDof(*facetCounts, v, 1); CHKERRQ(ierr);
                            goto addFacet;
                        }
                    }
                }
                continue;
            addFacet:
                if (facetIndex >= totalFacets) {
                    totalFacets = (PetscInt)((1 + totalFacets)*1.2);
                    ierr = PetscRealloc(sizeof(PetscInt)*totalFacets, &facetsArray); CHKERRQ(ierr);
                }
                facetsArray[facetIndex++] = f;
            }
        }
    }
    ierr = DMLabelDestroyIndex(facetLabel); CHKERRQ(ierr);
    ierr = ISRestoreIndices(cells, &cellsArray); CHKERRQ(ierr);
    PetscHashIDestroy(ht);

    ierr = PetscSectionSetUp(*facetCounts); CHKERRQ(ierr);
    ierr = PetscRealloc(sizeof(PetscInt)*facetIndex, &facetsArray); CHKERRQ(ierr);
    ierr = ISCreateGeneral(PETSC_COMM_SELF, facetIndex, facetsArray, PETSC_OWN_POINTER, facets); CHKERRQ(ierr);

    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchCreateCellPatchDiscretisationInfo"
/*
 * PCPatchCreateCellPatchDiscretisationInfo - Build the dof maps for cell patches
 *
 * Input Parameters:
 * + dm - The DMPlex object defining the mesh
 * . cellCounts - Section with counts of cells around each vertex
 * . cells - IS of the cell point indices of cells in each patch
 * . facetCounts - Section with counts of boundary facets for each cell patch
 * . facets - IS of the boundary facet point indices for each cell patch.
 * . cellNumbering - Section mapping plex cell points to Firedrake cell indices.
 * . dofsPerCell - number of dofs per cell.
 * - cellNodeMap - map from cells to dof indices (dofsPerCell * numCells)
 *
 * Output Parameters:
 * + dofs - IS of local dof numbers of each cell in the patch
 * . gtolCounts - Section with counts of dofs per cell patch
 * - gtol - IS mapping from global dofs to local dofs for each patch. 
 */
static PetscErrorCode PCPatchCreateCellPatchDiscretisationInfo(DM dm,
                                                               PetscSection cellCounts,
                                                               IS cells,
                                                               PetscSection facetCounts,
                                                               IS facets,
                                                               PetscSection cellNumbering,
                                                               const PetscInt dofsPerCell,
                                                               const PetscInt *cellNodeMap,
                                                               IS *dofs,
                                                               PetscSection *gtolCounts,
                                                               IS *gtol)
{
    PetscErrorCode  ierr;
    PetscInt        numCells;
    PetscInt        numDofs;
    PetscInt        numGlobalDofs;
    PetscInt        vStart, vEnd;
    const PetscInt *cellsArray;
    PetscInt       *newCellsArray = NULL;
    PetscInt       *dofsArray       = NULL;
    PetscInt       *globalDofsArray = NULL;
    PetscInt        globalIndex     = 0;
    PetscHashI      ht;
    PetscFunctionBegin;

    /* dofcounts section is cellcounts section * dofPerCell */
    ierr = PetscSectionGetStorageSize(cellCounts, &numCells); CHKERRQ(ierr);
    numDofs = numCells * dofsPerCell;
    ierr = PetscMalloc1(numDofs, &dofsArray); CHKERRQ(ierr);
    ierr = PetscMalloc1(numCells, &newCellsArray); CHKERRQ(ierr);
    ierr = PetscSectionGetChart(cellCounts, &vStart, &vEnd); CHKERRQ(ierr);
    ierr = PetscSectionCreate(PETSC_COMM_SELF, gtolCounts); CHKERRQ(ierr);
    ierr = PetscSectionSetChart(*gtolCounts, vStart, vEnd); CHKERRQ(ierr);

    ierr = ISGetIndices(cells, &cellsArray);
    PetscHashICreate(ht);
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        PetscInt dof, off;
        PetscInt localIndex = 0;
        PetscHashIClear(ht);
        ierr = PetscSectionGetDof(cellCounts, v, &dof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(cellCounts, v, &off); CHKERRQ(ierr);
        for ( PetscInt i = off; i < off + dof; i++ ) {
            /* Walk over the cells in this patch. */
            const PetscInt c = cellsArray[i];
            PetscInt cell;
            ierr = PetscSectionGetDof(cellNumbering, c, &cell); CHKERRQ(ierr);
            if ( cell <= 0 ) {
                SETERRQ(PETSC_COMM_WORLD, PETSC_ERR_ARG_OUTOFRANGE,
                        "Cell doesn't appear in cell numbering map");
            }
            ierr = PetscSectionGetOffset(cellNumbering, c, &cell); CHKERRQ(ierr);
            newCellsArray[i] = cell;
            for ( PetscInt j = 0; j < dofsPerCell; j++ ) {
                /* For each global dof, map it into contiguous local storage. */
                const PetscInt globalDof = cellNodeMap[cell*dofsPerCell + j];
                PetscInt localDof;
                PetscHashIMap(ht, globalDof, localDof);
                if (localDof == -1) {
                    localDof = localIndex++;
                    PetscHashIAdd(ht, globalDof, localDof);
                }
                if ( globalIndex >= numDofs ) {
                    SETERRQ(PETSC_COMM_WORLD, PETSC_ERR_ARG_OUTOFRANGE,
                            "Found more dofs than expected");
                }
                /* And store. */
                dofsArray[globalIndex++] = localDof;
            }
        }
        PetscHashISize(ht, dof);
        /* How many local dofs in this patch? */
        ierr = PetscSectionSetDof(*gtolCounts, v, dof); CHKERRQ(ierr);
    }
    ierr = PetscSectionSetUp(*gtolCounts); CHKERRQ(ierr);
    ierr = PetscSectionGetStorageSize(*gtolCounts, &numGlobalDofs); CHKERRQ(ierr);
    ierr = PetscMalloc1(numGlobalDofs, &globalDofsArray); CHKERRQ(ierr);

    /* Now populate the global to local map.  This could be merged
    * into the above loop if we were willing to deal with reallocs. */
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        PetscInt       dof, off;
        PetscHashIIter hi;
        PetscHashIClear(ht);
        ierr = PetscSectionGetDof(cellCounts, v, &dof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(cellCounts, v, &off); CHKERRQ(ierr);
        for ( PetscInt i = off; i < off + dof; i++ ) {
            /* Reconstruct mapping of global-to-local on this patch. */
            const PetscInt c = cellsArray[i];
            PetscInt cell;
            ierr = PetscSectionGetOffset(cellNumbering, c, &cell); CHKERRQ(ierr);
            for ( PetscInt j = 0; j < dofsPerCell; j++ ) {
                const PetscInt globalDof = cellNodeMap[cell*dofsPerCell + j];
                const PetscInt localDof = dofsArray[i*dofsPerCell + j];
                PetscHashIAdd(ht, globalDof, localDof);
            }
        }
        /* Shove it in the output data structure. */
        ierr = PetscSectionGetOffset(*gtolCounts, v, &off); CHKERRQ(ierr);
        PetscHashIIterBegin(ht, hi);
        while (!PetscHashIIterAtEnd(ht, hi)) {
            PetscInt globalDof, localDof;
            PetscHashIIterGetKeyVal(ht, hi, globalDof, localDof);
            if (globalDof >= 0) {
                globalDofsArray[off + localDof] = globalDof;
            }
            PetscHashIIterNext(ht, hi);
        }
    }
    ierr = ISRestoreIndices(cells, &cellsArray);

    /* Replace cell indices with firedrake-numbered ones. */
    ierr = ISGeneralSetIndices(cells, numCells, (const PetscInt *)newCellsArray, PETSC_OWN_POINTER); CHKERRQ(ierr);
    ierr = ISCreateGeneral(PETSC_COMM_SELF, numGlobalDofs, globalDofsArray, PETSC_OWN_POINTER, gtol); CHKERRQ(ierr);
    ierr = ISCreateGeneral(PETSC_COMM_SELF, numDofs, dofsArray, PETSC_OWN_POINTER, dofs); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchCreateCellPatchBCs"
static PetscErrorCode PCPatchCreateCellPatchBCs(DM dm,
                                                PetscInt numBcs,
                                                const PetscInt *bcNodes,
                                                PetscSection facetCounts,
                                                IS facets,
                                                PetscSection gtolCounts,
                                                IS gtol,
                                                PetscSection dofSection,
                                                PetscSection *bcCounts,
                                                IS *bcs)
{
    PetscErrorCode  ierr;
    PetscHashI      globalBcs;
    PetscHashI      localBcs;
    PetscHashI      patchDofs;
    PetscInt       *bcsArray = NULL;
    PetscInt        totalNumBcs;
    PetscInt        bcIndex  = 0;
    PetscInt        vStart, vEnd;
    PetscInt        closureSize;
    PetscInt       *closure  = NULL;
    const PetscInt *gtolArray;
    const PetscInt *facetsArray;
    PetscFunctionBegin;

    PetscHashICreate(globalBcs);
    for ( PetscInt i = 0; i < numBcs; i++ ) {
        PetscHashIAdd(globalBcs, bcNodes[i], 0);
    }

    PetscHashICreate(patchDofs);
    PetscHashICreate(localBcs);

    ierr = PetscSectionGetChart(facetCounts, &vStart, &vEnd); CHKERRQ(ierr);
    ierr = PetscSectionCreate(PETSC_COMM_SELF, bcCounts); CHKERRQ(ierr);
    ierr = PetscSectionSetChart(*bcCounts, vStart, vEnd); CHKERRQ(ierr);
    /* Guess at number of bcs */
    ierr = PetscSectionGetStorageSize(facetCounts, &totalNumBcs); CHKERRQ(ierr);
    ierr = PetscMalloc1(totalNumBcs, &bcsArray); CHKERRQ(ierr);

    ierr = ISGetIndices(gtol, &gtolArray); CHKERRQ(ierr);
    ierr = ISGetIndices(facets, &facetsArray); CHKERRQ(ierr);
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        PetscInt numBcs, dof, off;
        PetscHashIClear(patchDofs);
        PetscHashIClear(localBcs);
        ierr = PetscSectionGetDof(gtolCounts, v, &dof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(gtolCounts, v, &off); CHKERRQ(ierr);
        for ( PetscInt i = off; i < off + dof; i++ ) {
            PetscBool flg;
            const PetscInt globalDof = gtolArray[i];
            const PetscInt localDof = i - off;
            PetscHashIAdd(patchDofs, globalDof, localDof);
            PetscHashIHasKey(globalBcs, globalDof, flg);
            if (flg) {
                PetscHashIAdd(localBcs, localDof, 0);
            }
        }
        ierr = PetscSectionGetDof(facetCounts, v, &dof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(facetCounts, v, &off); CHKERRQ(ierr);
        for ( PetscInt i = off; i < off + dof; i++ ) {
            const PetscInt f = facetsArray[i];
            ierr = DMPlexGetTransitiveClosure(dm, f, PETSC_TRUE, &closureSize, &closure); CHKERRQ(ierr);
            for ( PetscInt ci = 0; ci < closureSize; ci++ ) {
                PetscInt ldof, loff;
                ierr = PetscSectionGetDof(dofSection, f, &ldof); CHKERRQ(ierr);
                ierr = PetscSectionGetOffset(dofSection, f, &loff); CHKERRQ(ierr);
                if ( ldof > 0 ) {
                    for ( PetscInt j = loff; j < ldof + loff; j++ ) {
                        PetscInt localDof;
                        PetscHashIMap(patchDofs, j, localDof);
                        if ( localDof == -1 ) {
                            SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_OUTOFRANGE,
                                    "Didn't find facet dof in patch dof\n");
                        }
                        PetscHashIAdd(localBcs, localDof, 0);
                    }
                }
            }
        }
        PetscHashISize(localBcs, numBcs);
        ierr = PetscSectionSetDof(*bcCounts, v, numBcs); CHKERRQ(ierr);
        /* OK, now we have a hash table with all the bcs indicated by
         * the facets and global bcs */
        if ( numBcs + bcIndex >= totalNumBcs ) {
            totalNumBcs = (PetscInt)((1 + numBcs + bcIndex)*2);
            ierr = PetscRealloc(sizeof(PetscInt)*totalNumBcs, &bcsArray); CHKERRQ(ierr);
        }
        ierr = PetscHashIGetKeys(localBcs, &bcIndex, bcsArray); CHKERRQ(ierr);
        ierr = PetscSortInt(numBcs, bcsArray + bcIndex - numBcs); CHKERRQ(ierr);
    }
    ierr = DMPlexRestoreTransitiveClosure(dm, 0, PETSC_TRUE, &closureSize, &closure); CHKERRQ(ierr);
    ierr = ISRestoreIndices(gtol, &gtolArray); CHKERRQ(ierr);
    ierr = ISRestoreIndices(facets, &facetsArray); CHKERRQ(ierr);
    PetscHashIDestroy(localBcs);
    PetscHashIDestroy(patchDofs);
    PetscHashIDestroy(globalBcs);

    ierr = PetscSectionSetUp(*bcCounts); CHKERRQ(ierr);
    ierr = PetscRealloc(sizeof(PetscInt)*bcIndex, &bcsArray); CHKERRQ(ierr);
    ierr = ISCreateGeneral(PETSC_COMM_SELF, bcIndex, bcsArray, PETSC_OWN_POINTER, bcs); CHKERRQ(ierr);

    PetscFunctionReturn(0);
}

static PetscErrorCode PCPatchCreateGlobalToLocalSF(PetscSF gtolSF,
                                                   PetscSection gtolCounts,
                                                   IS gtol,
                                                   PetscSF *sf)
{
    PetscErrorCode  ierr;
    PetscSF         localSF;
    PetscSFNode    *iremote;
    const PetscInt *gtolArray;
    PetscInt        nroots, nleaves;
    PetscFunctionBegin;

    ierr = PetscSFCreate(PetscObjectComm((PetscObject)gtolCounts), &localSF); CHKERRQ(ierr);
    ierr = PetscSFGetGraph(gtolSF, NULL, &nroots, NULL, NULL); CHKERRQ(ierr);
    ierr = PetscSectionGetStorageSize(gtolCounts, &nleaves); CHKERRQ(ierr);

    ierr = ISGetIndices(gtol, &gtolArray); CHKERRQ(ierr);

    ierr = PetscMalloc1(nleaves, &iremote); CHKERRQ(ierr);
    for ( PetscInt i = 0; i < nleaves; i++ ) {
        iremote[i].rank = 0;
        iremote[i].index = gtolArray[i];
    }
    ierr = ISRestoreIndices(gtol, &gtolArray); CHKERRQ(ierr);
    ierr = PetscSFSetGraph(localSF, nroots, nleaves, NULL, PETSC_OWN_POINTER,
                           iremote, PETSC_OWN_POINTER);
    ierr = PetscSFCompose(gtolSF, localSF, sf); CHKERRQ(ierr);
    ierr = PetscSFDestroy(&localSF); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCReset_PATCH"
static PetscErrorCode PCReset_PATCH(PC pc)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscInt        i;

    PetscFunctionBegin;
    ierr = DMDestroy(&patch->dm); CHKERRQ(ierr);
    ierr = PetscSFDestroy(&patch->globalToLocal); CHKERRQ(ierr);
    ierr = PetscSFDestroy(&patch->defaultSF); CHKERRQ(ierr);
    ierr = PetscSectionDestroy(&patch->dofSection); CHKERRQ(ierr);
    ierr = PetscSectionDestroy(&patch->cellCounts); CHKERRQ(ierr);
    ierr = PetscSectionDestroy(&patch->cellNumbering); CHKERRQ(ierr);
    ierr = PetscSectionDestroy(&patch->localToPatch); CHKERRQ(ierr);
    ierr = PetscSectionDestroy(&patch->bcCounts); CHKERRQ(ierr);
    ierr = ISDestroy(&patch->cells); CHKERRQ(ierr);
    ierr = ISDestroy(&patch->dofs); CHKERRQ(ierr);
    ierr = ISDestroy(&patch->bcs); CHKERRQ(ierr);

    if (patch->free_type) {
        ierr = MPI_Type_free(&patch->data_type); CHKERRQ(ierr);
        patch->data_type = MPI_DATATYPE_NULL; 
    }

    if (patch->ksp) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = KSPReset(patch->ksp[i]); CHKERRQ(ierr);
        }
    }

    ierr = VecDestroy(&patch->localX); CHKERRQ(ierr);
    ierr = VecDestroy(&patch->localY); CHKERRQ(ierr);
    if (patch->patchX) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = VecDestroy(patch->patchX + i); CHKERRQ(ierr);
        }
        ierr = PetscFree(patch->patchX); CHKERRQ(ierr);
    }
    if (patch->patchY) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = VecDestroy(patch->patchY + i); CHKERRQ(ierr);
        }
        ierr = PetscFree(patch->patchY); CHKERRQ(ierr);
    }
    if (patch->mat) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = MatDestroy(patch->mat + i); CHKERRQ(ierr);
        }
        ierr = PetscFree(patch->mat); CHKERRQ(ierr);
    }
    ierr = PetscFree(patch->sub_mat_type); CHKERRQ(ierr);

    patch->free_type = PETSC_FALSE;
    patch->numBcs = 0;
    patch->bcNodes = NULL;
    patch->bs = 0;
    patch->cellNodeMap = NULL;
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCDestroy_PATCH"
static PetscErrorCode PCDestroy_PATCH(PC pc)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscInt        i;

    PetscFunctionBegin;

    ierr = PCReset_PATCH(pc); CHKERRQ(ierr);
    if (patch->ksp) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = KSPDestroy(&patch->ksp[i]); CHKERRQ(ierr);
        }
        ierr = PetscFree(patch->ksp); CHKERRQ(ierr);
    }
    ierr = PetscFree(pc->data); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchCreateMatrix"
static PetscErrorCode PCPatchCreateMatrix(PC pc, Vec x, Vec y, Mat *mat)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscInt        csize, rsize, cbs, rbs;

    PetscFunctionBegin;
    ierr = VecGetSize(x, &csize); CHKERRQ(ierr);
    ierr = VecGetBlockSize(x, &cbs); CHKERRQ(ierr);
    ierr = VecGetSize(y, &rsize); CHKERRQ(ierr);
    ierr = VecGetBlockSize(y, &rbs); CHKERRQ(ierr);
    ierr = MatCreate(PETSC_COMM_SELF, mat); CHKERRQ(ierr);
    if (patch->sub_mat_type) {
        ierr = MatSetType(*mat, patch->sub_mat_type); CHKERRQ(ierr);
    }
    ierr = MatSetSizes(*mat, rsize, csize, rsize, csize); CHKERRQ(ierr);
    ierr = MatSetBlockSizes(*mat, rbs, cbs); CHKERRQ(ierr);
    ierr = MatSetUp(*mat); CHKERRQ(ierr);
    
    PetscFunctionReturn(0);
}


#undef __FUNCT__
#define __FUNCT__ "PCPatchComputeOperator"
static PetscErrorCode PCPatchComputeOperator(PC pc, Mat mat, PetscInt which)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    const PetscInt *dofsArray;
    const PetscInt *cellsArray;
    PetscInt        ncell, offset, pStart, pEnd;

    PetscFunctionBegin;

    if (!patch->usercomputeop) {
        SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_WRONGSTATE, "Must call PCPatchSetComputeOperator() to set user callback\n");
    }
    ierr = ISGetIndices(patch->dofs, &dofsArray); CHKERRQ(ierr);
    ierr = ISGetIndices(patch->cells, &cellsArray); CHKERRQ(ierr);
    ierr = PetscSectionGetChart(patch->cellCounts, &pStart, &pEnd); CHKERRQ(ierr);

    which += pStart;
    if (which >= pEnd) {
        SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_OUTOFRANGE, "Asked for operator index is invalid\n"); CHKERRQ(ierr);
    }

    ierr = PetscSectionGetDof(patch->cellCounts, which, &ncell); CHKERRQ(ierr);
    if (ncell <= 0) {
        SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_OUTOFRANGE, "Expecting positive number of patch cells\n"); CHKERRQ(ierr);
    }
    ierr = PetscSectionGetOffset(patch->cellCounts, which, &offset); CHKERRQ(ierr);
    PetscStackPush("PCPatch user callback");
    ierr = patch->usercomputeop(pc, mat, ncell, cellsArray + offset, ncell*patch->nodesPerCell, dofsArray + offset*patch->nodesPerCell, patch->usercomputectx); CHKERRQ(ierr);
    PetscStackPop;
    ierr = ISRestoreIndices(patch->dofs, &dofsArray); CHKERRQ(ierr);
    ierr = ISRestoreIndices(patch->cells, &cellsArray); CHKERRQ(ierr);
    /* Apply boundary conditions.  Could also do this through the local_to_patch guy. */
    /* ierr = MatZeroRowsColumnsIS(mat, patch->bcNodes[which-pStart], (PetscScalar)1.0, NULL, NULL); CHKERRQ(ierr); */
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCSetUp_PATCH"
static PetscErrorCode PCSetUp_PATCH(PC pc)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    const char     *prefix;

    PetscFunctionBegin;

    if (!pc->setupcalled) {
        PetscSection facetCounts, gtolCounts;
        IS           facets, gtol;
        PetscInt     pStart, pEnd;
        PetscInt     localSize;
        switch (patch->bs) {
        case 1:
            patch->data_type = MPIU_SCALAR;
            break;
        case 2:
            patch->data_type = MPIU_2SCALAR;
            break;
        default:
            ierr = MPI_Type_contiguous(patch->bs, MPIU_SCALAR, &patch->data_type); CHKERRQ(ierr);
            ierr = MPI_Type_commit(&patch->data_type); CHKERRQ(ierr);
            patch->free_type = PETSC_TRUE;
        }
        ierr = PCPatchCreateCellPatches(patch->dm, &patch->cellCounts, &patch->cells); CHKERRQ(ierr);
        ierr = PetscSectionGetChart(patch->cellCounts, &pStart, &pEnd); CHKERRQ(ierr);
        patch->npatch = pEnd - pStart;
        ierr = PCPatchCreateCellPatchFacets(patch->dm, patch->cellCounts, patch->cells, &facetCounts, &facets); CHKERRQ(ierr);
        ierr = PCPatchCreateCellPatchDiscretisationInfo(patch->dm, patch->cellCounts, patch->cells, facetCounts,
                                                        facets, patch->cellNumbering,
                                                        patch->nodesPerCell, patch->cellNodeMap,
                                                        &patch->dofs, &gtolCounts, &gtol); CHKERRQ(ierr);
        ierr = PCPatchCreateCellPatchBCs(patch->dm, patch->numBcs, patch->bcNodes,
                                         facetCounts, facets, gtolCounts, gtol,
                                         patch->dofSection, &patch->bcCounts, &patch->bcs); CHKERRQ(ierr);
        ierr = PetscSectionDestroy(&facetCounts); CHKERRQ(ierr);
        ierr = ISDestroy(&facets); CHKERRQ(ierr);

        ierr = PCPatchCreateGlobalToLocalSF(patch->defaultSF, gtolCounts, gtol,
                                            &patch->globalToLocal); CHKERRQ(ierr);

        ierr = ISDestroy(&gtol); CHKERRQ(ierr);
        /* OK, now build the work vectors */
        ierr = PetscSectionGetStorageSize(gtolCounts, &localSize); CHKERRQ(ierr);
        localSize *= patch->bs;
        ierr = VecCreateSeq(PETSC_COMM_SELF, localSize, &patch->localX); CHKERRQ(ierr);
        ierr = VecSetBlockSize(patch->localX, patch->bs); CHKERRQ(ierr);
        ierr = VecSetUp(patch->localX); CHKERRQ(ierr);
        ierr = VecDuplicate(patch->localX, &patch->localY); CHKERRQ(ierr);
        ierr = PetscMalloc1(patch->npatch, &patch->patchX); CHKERRQ(ierr);
        ierr = PetscMalloc1(patch->npatch, &patch->patchY); CHKERRQ(ierr);
        for ( PetscInt i = pStart; i < pEnd; i++ ) {
            PetscInt dof;
            ierr = PetscSectionGetDof(gtolCounts, i, &dof); CHKERRQ(ierr);
            if ( dof > 0 ) {
                ierr = VecCreateSeqWithArray(PETSC_COMM_SELF, patch->bs,
                                             dof*patch->bs, NULL, &patch->patchX[i - pStart]); CHKERRQ(ierr);
                ierr = VecCreateSeqWithArray(PETSC_COMM_SELF, patch->bs,
                                             dof*patch->bs, NULL, &patch->patchY[i - pStart]); CHKERRQ(ierr);
            }
        }
        ierr = PetscMalloc1(patch->npatch, &patch->ksp); CHKERRQ(ierr);
        ierr = PCGetOptionsPrefix(pc, &prefix); CHKERRQ(ierr);
        patch->localToPatch = gtolCounts;
        ierr = PetscObjectReference((PetscObject)gtolCounts); CHKERRQ(ierr);
        ierr = PetscSectionDestroy(&gtolCounts); CHKERRQ(ierr);
        ierr = ISDestroy(&gtol); CHKERRQ(ierr);
        for ( PetscInt i = 0; i < patch->npatch; i++ ) {
            ierr = KSPCreate(PETSC_COMM_SELF, patch->ksp + i); CHKERRQ(ierr);
            ierr = KSPSetOptionsPrefix(patch->ksp[i], prefix); CHKERRQ(ierr);
            ierr = KSPAppendOptionsPrefix(patch->ksp[i], "sub_"); CHKERRQ(ierr);
        }
        if (patch->save_operators) {
            ierr = PetscMalloc1(patch->npatch, &patch->mat); CHKERRQ(ierr);
            for ( PetscInt i = 0; i < patch->npatch; i++ ) {
                ierr = PCPatchCreateMatrix(pc, patch->patchX[i], patch->patchY[i], patch->mat + i); CHKERRQ(ierr);
            }
        }
    }
    if (patch->save_operators) {
        for ( PetscInt i = 0; i < patch->npatch; i++ ) {
            ierr = PCPatchComputeOperator(pc, patch->mat[i], i); CHKERRQ(ierr);
            ierr = KSPSetOperators(patch->ksp[i], patch->mat[i], NULL); CHKERRQ(ierr);
        }
    }
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCApply_PATCH"
static PetscErrorCode PCApply_PATCH(PC pc, Vec x, Vec y)
{
    PetscErrorCode     ierr;
    PC_PATCH          *patch   = (PC_PATCH *)pc->data;
    const PetscScalar *globalX = NULL;
    PetscScalar       *localX  = NULL;
    PetscScalar       *localY  = NULL;
    PetscScalar       *globalY = NULL;
    PetscScalar       *patchX  = NULL;
    PetscInt           pStart;
    
    PetscFunctionBegin;

    ierr = VecGetArrayRead(x, &globalX); CHKERRQ(ierr);
    ierr = VecGetArray(patch->localX, &localX); CHKERRQ(ierr);
    /* Scatter from global space into overlapped local spaces */
    ierr = PetscSFBcastBegin(patch->globalToLocal, patch->data_type, globalX, localX); CHKERRQ(ierr);
    ierr = PetscSFBcastEnd(patch->globalToLocal, patch->data_type, globalX, localX); CHKERRQ(ierr);
    ierr = VecRestoreArrayRead(x, &globalX); CHKERRQ(ierr);
    ierr = VecRestoreArray(patch->localX, &localX); CHKERRQ(ierr);
    ierr = PetscSectionGetChart(patch->localToPatch, &pStart, NULL); CHKERRQ(ierr);
    ierr = VecGetArrayRead(patch->localX, (const PetscScalar **)&localX); CHKERRQ(ierr);
    ierr = VecGetArray(patch->localY, &localY); CHKERRQ(ierr);
    for ( PetscInt i = 0; i < patch->npatch; i++ ) {
        PetscInt start, len;
        ierr = PetscSectionGetDof(patch->localToPatch, i + pStart, &len); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(patch->localToPatch, i + pStart, &start); CHKERRQ(ierr);
        if ( len <= 0 ) {
            SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_SIZ, "Did not find correct length for localToPatch mapping\n"); CHKERRQ(ierr);
        }
        start *= patch->bs;
        ierr = VecPlaceArray(patch->patchX[i], localX + start); CHKERRQ(ierr);
        ierr = VecPlaceArray(patch->patchY[i], localY + start); CHKERRQ(ierr);
        /* TODO: Do we need different scatters for X and Y? */
        ierr = VecGetArray(patch->patchX[i], &patchX); CHKERRQ(ierr);
        /* Apply bcs to patchX (zero entries) */
        ierr = VecRestoreArray(patch->patchX[i], &patchX); CHKERRQ(ierr);
        if (!patch->save_operators) {
            Mat mat;
            ierr = PCPatchCreateMatrix(pc, patch->patchX[i], patch->patchY[i], &mat); CHKERRQ(ierr);
            /* Populate operator here. */
            ierr = PCPatchComputeOperator(pc, mat, i); CHKERRQ(ierr);
            ierr = KSPSetOperators(patch->ksp[i], mat, NULL);
            /* Drop reference so the KSPSetOperators below will blow it away. */
            ierr = MatDestroy(&mat); CHKERRQ(ierr);
        }
        
        // ierr = KSPSolve(patch->ksp[i], patch->patchX[i], patch->patchY[i]); CHKERRQ(ierr);

        if (!patch->save_operators) {
            ierr = KSPSetOperators(patch->ksp[i], NULL, NULL); CHKERRQ(ierr);
        }
        ierr = VecCopy(patch->patchX[i], patch->patchY[i]); CHKERRQ(ierr);
        ierr = VecResetArray(patch->patchX[i]); CHKERRQ(ierr);
        ierr = VecResetArray(patch->patchY[i]); CHKERRQ(ierr);
    }
    ierr = VecRestoreArrayRead(patch->localX, (const PetscScalar **)&localX); CHKERRQ(ierr);
    ierr = VecRestoreArray(patch->localY, &localY); CHKERRQ(ierr);
    /* Now patch->localY contains the solution of the patch solves, so
     * we need to combine them all.  This hardcodes an ADDITIVE
     * combination right now.  If one wanted multiplicative, the
     * scatter/gather stuff would have to be reworked a bit. */
    ierr = VecSet(y, 0.0); CHKERRQ(ierr);
    ierr = VecGetArrayRead(patch->localY, (const PetscScalar **)&localY); CHKERRQ(ierr);
    ierr = VecGetArray(y, &globalY); CHKERRQ(ierr);
    ierr = PetscSFReduceBegin(patch->globalToLocal, patch->data_type, localY, globalY, MPI_SUM); CHKERRQ(ierr);
    ierr = PetscSFReduceEnd(patch->globalToLocal, patch->data_type, localY, globalY, MPI_SUM); CHKERRQ(ierr);
    ierr = VecRestoreArrayRead(patch->localY, (const PetscScalar **)&localY); CHKERRQ(ierr);
    ierr = VecRestoreArray(y, &globalY); CHKERRQ(ierr);

    /* Now we need to send the global BC values through */
    /* 
     * ierr = VecScatterBegin(globalBcScatter, x, y, INSERT_VALUES, SCATTER_FORWARD); CHKERRQ(ierr);
     * ierr = VecScatterEnd(globalBcScatter, x, y, INSERT_VALUES, SCATTER_FORWARD); CHKERRQ(ierr);
     */
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCSetUpOnBlocks_PATCH"
static PetscErrorCode PCSetUpOnBlocks_PATCH(PC pc)
{
  PC_PATCH           *patch = (PC_PATCH*)pc->data;
  PetscErrorCode      ierr;
  PetscInt            i;
  KSPConvergedReason  reason;

  PetscFunctionBegin;
  PetscFunctionReturn(0);
  for (i=0; i<patch->npatch; i++) {
    ierr = KSPSetUp(patch->ksp[i]); CHKERRQ(ierr);
    ierr = KSPGetConvergedReason(patch->ksp[i], &reason); CHKERRQ(ierr);
    if (reason == KSP_DIVERGED_PCSETUP_FAILED) {
      pc->failedreason = PC_SUBPC_ERROR;
    }
  }
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCSetFromOptions_PATCH"
static PetscErrorCode PCSetFromOptions_PATCH(PetscOptionItems *PetscOptionsObject, PC pc)
{
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscErrorCode  ierr;
    PetscBool       flg;
    char            sub_mat_type[256];

    PetscFunctionBegin;
    ierr = PetscOptionsHead(PetscOptionsObject, "Vertex-patch Additive Schwarz options"); CHKERRQ(ierr);

    ierr = PetscOptionsBool("-pc_patch_save_operators", "Store all patch operators for lifetime of PC?",
                            "PCPatchSetSaveOperators", patch->save_operators, &patch->save_operators, &flg); CHKERRQ(ierr);

    ierr = PetscOptionsFList("-pc_patch_sub_mat_type", "Matrix type for patch solves", "PCPatchSetSubMatType",MatList, NULL, sub_mat_type, 256, &flg); CHKERRQ(ierr);
    if (flg) {
        ierr = PCPatchSetSubMatType(pc, sub_mat_type); CHKERRQ(ierr);
    }
    ierr = PetscOptionsTail(); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}
#undef __FUNCT__
#define __FUNCT__ "PCView_PATCH"
static PetscErrorCode PCView_PATCH(PC pc, PetscViewer viewer)
{
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscErrorCode  ierr;
    PetscMPIInt     rank;
    PetscBool       isascii;
    PetscViewer     sviewer;
    PetscFunctionBegin;
    ierr = PetscObjectTypeCompare((PetscObject)viewer,PETSCVIEWERASCII,&isascii);CHKERRQ(ierr);

    ierr = MPI_Comm_rank(PetscObjectComm((PetscObject)pc),&rank);CHKERRQ(ierr);
    if (!isascii) {
        PetscFunctionReturn(0);
    }
    ierr = PetscViewerASCIIPushTab(viewer); CHKERRQ(ierr);
    ierr = PetscViewerASCIIPrintf(viewer, "Vertex-patch Additive Schwarz with %d patches\n", patch->npatch); CHKERRQ(ierr);
    if (!patch->save_operators) {
        ierr = PetscViewerASCIIPrintf(viewer, "Not saving patch operators (rebuilt every PCApply)\n"); CHKERRQ(ierr);
    } else {
        ierr = PetscViewerASCIIPrintf(viewer, "Saving patch operators (rebuilt every PCSetUp)\n"); CHKERRQ(ierr);
    }
    ierr = PetscViewerASCIIPrintf(viewer, "DM used to define patches:\n"); CHKERRQ(ierr);
    ierr = PetscViewerASCIIPushTab(viewer); CHKERRQ(ierr);
    if (patch->dm) {
        ierr = DMView(patch->dm, viewer); CHKERRQ(ierr);
    } else {
        ierr = PetscViewerASCIIPrintf(viewer, "DM not yet set.\n"); CHKERRQ(ierr);
    }
    ierr = PetscViewerASCIIPopTab(viewer); CHKERRQ(ierr);
    ierr = PetscViewerASCIIPrintf(viewer, "KSP on patches (all same):\n"); CHKERRQ(ierr);
    if (patch->ksp) {
        ierr = PetscViewerGetSubViewer(viewer, PETSC_COMM_SELF, &sviewer); CHKERRQ(ierr);
        if (!rank) {
            ierr = PetscViewerASCIIPushTab(sviewer); CHKERRQ(ierr);
            ierr = KSPView(patch->ksp[0], sviewer); CHKERRQ(ierr);
            ierr = PetscViewerASCIIPopTab(sviewer); CHKERRQ(ierr);
        }
        ierr = PetscViewerRestoreSubViewer(viewer, PETSC_COMM_SELF, &sviewer); CHKERRQ(ierr);
    } else {
        ierr = PetscViewerASCIIPushTab(viewer); CHKERRQ(ierr);
        ierr = PetscViewerASCIIPrintf(viewer, "KSP not yet set.\n"); CHKERRQ(ierr);
        ierr = PetscViewerASCIIPopTab(viewer); CHKERRQ(ierr);
    }

    ierr = PetscViewerASCIIPopTab(viewer); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

        
#undef __FUNCT__
#define __FUNCT__ "PCCreate_PATCH"
PETSC_EXTERN PetscErrorCode PCCreate_PATCH(PC pc)
{
    PetscErrorCode ierr;
    PC_PATCH       *patch;

    PetscFunctionBegin;

    ierr = PetscNewLog(pc, &patch); CHKERRQ(ierr);

    patch->sub_mat_type      = NULL;
    pc->data                 = (void *)patch;
    pc->ops->apply           = PCApply_PATCH;
    pc->ops->applytranspose  = 0; //PCApplyTranspose_PATCH;
    pc->ops->setup           = PCSetUp_PATCH;
    pc->ops->reset           = PCReset_PATCH;
    pc->ops->destroy         = PCDestroy_PATCH;
    pc->ops->setfromoptions  = PCSetFromOptions_PATCH;
    pc->ops->setuponblocks   = PCSetUpOnBlocks_PATCH;
    pc->ops->view            = PCView_PATCH;
    pc->ops->applyrichardson = 0;

    PetscFunctionReturn(0);
}
