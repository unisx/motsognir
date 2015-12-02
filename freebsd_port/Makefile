# $FreeBSD$

PORTNAME=       motsognir
PORTVERSION=    1.0.4
CATEGORIES=     net
LICENSE=        GPLv3
MASTER_SITES=	SF/${PORTNAME}/v${PORTVERSION}

MAINTAINER=     mateusz@viste.fr
COMMENT=        A full-featured gopher server without dependencies
NO_STAGE=       yes

do-install:
	${INSTALL_PROGRAM} ${WRKSRC}/motsognir ${PREFIX}/bin/motsognir
	${INSTALL_DATA} ${WRKSRC}/motsognir.conf ${PREFIX}/etc/motsognir.conf
	${INSTALL_SCRIPT} ${WRKSRC}/rc_d_motsognir ${PREFIX}/etc/rc.d/motsognir

.include <bsd.port.mk>
