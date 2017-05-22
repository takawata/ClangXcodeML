#ifndef XCODEMLNAMEELEM_H
#define XCODEMLNAMEELEM_H

xmlNodePtr makeNameNode(TypeTableInfo&, const clang::NamedDecl*);
xmlNodePtr makeIdNodeForCXXMethodDecl(TypeTableInfo&, const clang::CXXMethodDecl*);
xmlNodePtr makeIdNodeForFieldDecl(TypeTableInfo&, const clang::FieldDecl*);

#endif /*! XCODEMLNAMEELEM_H */
