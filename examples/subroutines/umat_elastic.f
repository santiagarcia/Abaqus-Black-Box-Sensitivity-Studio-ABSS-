C     ABSS example UMAT: small-strain isotropic linear elasticity (3D and plane strain).
C     PROPS(1) = E, PROPS(2) = nu.  STATEV(1) = equivalent strain measure (for SDV output tests).
C     Used by examples/models/umat_props.inp:
C        abaqus job=umat_props user=umat_elastic.f
C     ABSS treats this routine as opaque: it only perturbs PROPS(k) in the input deck.
C     Not compiled on the development machine (no Intel Fortran); see docs/ABAQUS_REQUIRED_TESTS.md.
      SUBROUTINE UMAT(STRESS,STATEV,DDSDDE,SSE,SPD,SCD,
     1 RPL,DDSDDT,DRPLDE,DRPLDT,
     2 STRAN,DSTRAN,TIME,DTIME,TEMP,DTEMP,PREDEF,DPRED,CMNAME,
     3 NDI,NSHR,NTENS,NSTATV,PROPS,NPROPS,COORDS,DROT,PNEWDT,
     4 CELENT,DFGRD0,DFGRD1,NOEL,NPT,LAYER,KSPT,JSTEP,KINC)
      INCLUDE 'ABA_PARAM.INC'
      CHARACTER*80 CMNAME
      DIMENSION STRESS(NTENS),STATEV(NSTATV),
     1 DDSDDE(NTENS,NTENS),DDSDDT(NTENS),DRPLDE(NTENS),
     2 STRAN(NTENS),DSTRAN(NTENS),TIME(2),PREDEF(1),DPRED(1),
     3 PROPS(NPROPS),COORDS(3),DROT(3,3),DFGRD0(3,3),DFGRD1(3,3),
     4 JSTEP(4)
      DOUBLE PRECISION E, ANU, ALAM, AMU, EPS
      INTEGER I, J
      E    = PROPS(1)
      ANU  = PROPS(2)
      ALAM = E*ANU/((1.D0+ANU)*(1.D0-2.D0*ANU))
      AMU  = E/(2.D0*(1.D0+ANU))
      DO I = 1, NTENS
        DO J = 1, NTENS
          DDSDDE(I,J) = 0.D0
        END DO
      END DO
      DO I = 1, NDI
        DO J = 1, NDI
          DDSDDE(I,J) = ALAM
        END DO
        DDSDDE(I,I) = ALAM + 2.D0*AMU
      END DO
      DO I = NDI+1, NTENS
        DDSDDE(I,I) = AMU
      END DO
      DO I = 1, NTENS
        DO J = 1, NTENS
          STRESS(I) = STRESS(I) + DDSDDE(I,J)*DSTRAN(J)
        END DO
      END DO
      EPS = 0.D0
      DO I = 1, NTENS
        EPS = EPS + (STRAN(I)+DSTRAN(I))**2
      END DO
      IF (NSTATV .GE. 1) STATEV(1) = SQRT(EPS)
      RETURN
      END
