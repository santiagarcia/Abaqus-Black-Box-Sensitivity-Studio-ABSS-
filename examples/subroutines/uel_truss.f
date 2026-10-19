C     ABSS example UEL: 2-node, 2D linear truss (small displacement).
C     PROPS(1) = E, PROPS(2) = A ; JPROPS(1) = option flag (unused, demonstrates an integer property).
C     Residual convention (Abaqus UEL): RHS = -(internal force) for the element, AMATRX = stiffness.
C     Used by examples/models/uel_props.inp:   abaqus job=uel_props user=uel_truss.f
C     ABSS treats this routine as opaque: it only perturbs PROPS(k) in *UEL PROPERTY.
C     Not compiled on the development machine (no Intel Fortran); see docs/ABAQUS_REQUIRED_TESTS.md.
      SUBROUTINE UEL(RHS,AMATRX,SVARS,ENERGY,NDOFEL,NRHS,NSVARS,
     1 PROPS,NPROPS,COORDS,MCRD,NNODE,U,DU,V,A,JTYPE,TIME,DTIME,
     2 KSTEP,KINC,JELEM,PARAMS,NDLOAD,JDLTYP,ADLMAG,PREDEF,NPREDF,
     3 LFLAGS,MLVARX,DDLMAG,MDLOAD,PNEWDT,JPROPS,NJPROP,PERIOD)
      INCLUDE 'ABA_PARAM.INC'
      DIMENSION RHS(MLVARX,*),AMATRX(NDOFEL,NDOFEL),PROPS(*),
     1 SVARS(*),ENERGY(8),COORDS(MCRD,NNODE),U(NDOFEL),
     2 DU(MLVARX,*),V(NDOFEL),A(NDOFEL),TIME(2),PARAMS(*),
     3 JDLTYP(MDLOAD,*),ADLMAG(MDLOAD,*),DDLMAG(MDLOAD,*),
     4 PREDEF(2,NPREDF,NNODE),LFLAGS(*),JPROPS(*)
      DOUBLE PRECISION E, AREA, DX, DY, AL, C, S, AK, B(4), F
      INTEGER I, J
      E    = PROPS(1)
      AREA = PROPS(2)
      DX = COORDS(1,2) - COORDS(1,1)
      DY = COORDS(2,2) - COORDS(2,1)
      AL = SQRT(DX*DX + DY*DY)
      C  = DX/AL
      S  = DY/AL
      AK = E*AREA/AL
      B(1) = -C
      B(2) = -S
      B(3) =  C
      B(4) =  S
      F = 0.D0
      DO I = 1, 4
        F = F + B(I)*U(I)
      END DO
      DO I = 1, NDOFEL
        RHS(I,1) = -AK*B(I)*F
        DO J = 1, NDOFEL
          AMATRX(I,J) = AK*B(I)*B(J)
        END DO
      END DO
      IF (NSVARS .GE. 1) SVARS(1) = AK*F
      RETURN
      END
