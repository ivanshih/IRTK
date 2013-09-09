/*=========================================================================

 Library   : Image Registration Toolkit (IRTK)
 Module    : $Id: irtkReconstructionb0.cc 837 2013-05-07 12:55:31Z mm3 $
 Copyright : Imperial College, Department of Computing
 Visual Information Processing (VIP), 2011 onwards
 Date      : $Date: 2013-05-07 13:55:31 +0100 (Tue, 07 May 2013) $
 Version   : $Revision: 837 $
 Changes   : $Author: mm3 $

 =========================================================================*/

#include <irtkReconstruction.h>
#include <irtkReconstructionb0.h>
#include <irtkResampling.h>
#include <irtkRegistration.h>
#include <irtkImageRigidRegistration.h>
#include <irtkImageRigidRegistrationWithPadding.h>
#include <irtkImageAffineRegistrationWithPadding.h>
#include <irtkImageFreeFormRegistrationWithPadding.h>
#include <irtkTransformation.h>

void irtkReconstructionb0::StackRegistrations(vector<irtkRealImage>& stacks,
		vector<irtkRigidTransformation>& stack_transformations)
{
	//rigid registration object
	irtkImageRigidRegistrationWithPadding registration;
	//buffer to create the name
	char buffer[256];

	//template is set as the target
	irtkGreyImage target = _reconstructed;
	//target needs to be masked before registration
	if(_debug)
	  target.Write("target-nomask.nii.gz");
	if (_have_mask)
	{
		double x, y, z;
		for (int i = 0; i < target.GetX(); i++)
			for (int j = 0; j < target.GetY(); j++)
				for (int k = 0; k < target.GetZ(); k++)
				{
					//image coordinates of the target
					x = i;
					y = j;
					z = k;
					//change to world coordinates
					target.ImageToWorld(x, y, z);
					//change to mask image coordinates - mask is aligned with target
					_mask.WorldToImage(x, y, z);
					x = round(x);
					y = round(y);
					z = round(z);
					//if the voxel is outside mask ROI set it to -1 (padding value)
					if ((x >= 0) && (x < _mask.GetX()) && (y >= 0) && (y < _mask.GetY()) && (z >= 0)
							&& (z < _mask.GetZ()))
							{
						if (_mask(x, y, z) == 0)
							target(i, j, k) = 0;
					}
					else
						target(i, j, k) = 0;
				}
	}

        if(_debug)
          target.Write("target.nii.gz");
        irtkRigidTransformation offset;
	ResetOrigin(target,offset);

	//register all stacks to the target
	for (int i = 0; i < (int)stacks.size(); i++)
	{
		//set target and source (need to be converted to irtkGreyImage)
		irtkGreyImage source = stacks[i];

               //include offset in trasformation	
		irtkMatrix mo = offset.GetMatrix();
		irtkMatrix m = stack_transformations[i].GetMatrix();
		m=m*mo;
		stack_transformations[i].PutMatrix(m);

		//perform rigid registration
		registration.SetInput(&target, &source);
		registration.SetOutput(&stack_transformations[i]);
		registration.GuessParameterThickSlices();
		registration.SetTargetPadding(0);
		registration.Run();
		
		mo.Invert();
		m = stack_transformations[i].GetMatrix();
		m=m*mo;
		stack_transformations[i].PutMatrix(m);


		//save volumetric registrations
		if (_debug)
		{
			registration.irtkImageRegistration::Write((char *) "parout-volume.rreg");
			sprintf(buffer, "stack-transformation%i.dof.gz", i);
			stack_transformations[i].irtkTransformation::Write(buffer);
			sprintf(buffer, "stack%i.nii.gz", i);
			stacks[i].Write(buffer);
		}
	}
}

void irtkReconstructionb0::SetT2Template(irtkRealImage T2)
{
  irtkRealImage t2template = _reconstructed;
  t2template=0;
  irtkRigidTransformation tr;
  
  irtkImageTransformation *imagetransformation = new irtkImageTransformation;
  irtkImageFunction *interpolator = new irtkLinearInterpolateImageFunction;
  imagetransformation->SetInput(&T2, &tr);
  imagetransformation->SetOutput(&t2template);
  //target contains zeros, need padding -1
  imagetransformation->PutTargetPaddingValue(-1);
  //need to fill voxels in target where there is no info from source with zeroes
  imagetransformation->PutSourcePaddingValue(0);
  imagetransformation->PutInterpolator(interpolator);
  imagetransformation->Run();
  
  _reconstructed = t2template;
  _reconstructed.Write("t2template.nii.gz");

}

irtkRealImage irtkReconstructionb0::AlignT2Template(irtkRealImage T2, double sigma)
{
  irtkImageRigidRegistrationWithPadding registration;
  irtkRigidTransformation offset,tr;
  irtkGreyImage target = _reconstructed;
  ResetOrigin(target,offset);

  if (sigma>0)
  {
    irtkGaussianBlurringWithPadding<irtkRealPixel> gb(sigma,0);
    gb.SetInput(&T2);
    gb.SetOutput(&T2);
    gb.Run();
  }
  irtkGreyImage source = T2;
	
  //include offset in trasformation	
  irtkMatrix mo = offset.GetMatrix();
  irtkMatrix m = tr.GetMatrix();
  m=m*mo;
  tr.PutMatrix(m);
  registration.SetInput(&target, &source);
  registration.SetOutput(&tr);
  registration.GuessParameter();
  registration.SetTargetPadding(0);
  registration.Run();
  //undo the offset
  mo.Invert();
  m = tr.GetMatrix();
  m=m*mo;
  tr.PutMatrix(m);
  
  tr.irtkTransformation::Write("tr.dof");
  
  //transform T2
  irtkRealImage t2template = _reconstructed;
  t2template=0;
  
  irtkImageTransformation *imagetransformation = new irtkImageTransformation;
  irtkImageFunction *interpolator = new irtkLinearInterpolateImageFunction;
  imagetransformation->SetInput(&T2, &tr);
  imagetransformation->SetOutput(&t2template);
  //target contains zeros, need padding -1
  imagetransformation->PutTargetPaddingValue(-1);
  //need to fill voxels in target where there is no info from source with zeroes
  imagetransformation->PutSourcePaddingValue(0);
  imagetransformation->PutInterpolator(interpolator);
  imagetransformation->Run();
  
  return t2template;

}


irtkRealImage irtkReconstructionb0::AdjustOrientation(irtkRealImage &image, bool swap)
{
  ////////////////////////////////////////////////////////////////////////////////////////////////
  //change orientation of the image so that we can use only sy, syx and syz affine parameters   //
  //as given by image coordinate system. This means extracting rotation (and reflection)        //
  //from image header and swapping x and y axis                                                 //
  ////////////////////////////////////////////////////////////////////////////////////////////////
  
  //image attributes
  irtkImageAttributes attr = image.GetImageAttributes();
    
  //set orientation in preparation for distortion correction
  
  if (swap)
  {
    attr._xaxis[0] = 0;
    attr._xaxis[1] = 1;
    attr._yaxis[0] = 1;
    attr._yaxis[1] = 0;
  }
  else
  {
    attr._xaxis[0] = 1;
    attr._xaxis[1] = 0;
    attr._yaxis[0] = 0;
    attr._yaxis[1] = 1;    
  }
  
  attr._xaxis[2] = 0;
  attr._yaxis[2] = 0;
  attr._zaxis[0] = 0;
  attr._zaxis[1] = 0;
  attr._zaxis[2] = 1;
    
  //reset origin
  attr._xorigin = 0;
  attr._yorigin = 0;
  attr._zorigin = 0;
  
  //create image with identity orientation and zero origin
  irtkRealImage newimage(attr);
    
  for (int i=0; i<attr._x; i++)
    for (int j=0; j<attr._y; j++)
      for (int k=0; k<attr._z; k++)
        for (int l=0; l<attr._t; l++)
          newimage(i,j,k,l)=image(i,j,k,l);
  
  return newimage;
}

irtkAffineTransformation irtkReconstructionb0::AdjustOrientationTransformation(irtkRealImage &image, bool swap)
{
  ////////////////////////////////////////////////////////////////////////////////////////////////
  //give transformation of the image with changed orientation (for purpose of distortion        //
  //correction, so that we can use only sy, syx and syz affine parameters) to the original      //
  //image                                                                                       //
  ////////////////////////////////////////////////////////////////////////////////////////////////
  
  //image attributes
  irtkImageAttributes attr = image.GetImageAttributes();
  
  irtkMatrix orient(4,4);
  orient.Ident();
  
  //find orientation axis
  //transposing equals inverting because it is rotation matrix
  orient(0, 0) = attr._xaxis[0];
  orient(0, 1) = attr._xaxis[1];
  orient(0, 2) = attr._xaxis[2];
  orient(1, 0) = attr._yaxis[0];
  orient(1, 1) = attr._yaxis[1];
  orient(1, 2) = attr._yaxis[2];
  orient(2, 0) = attr._zaxis[0];
  orient(2, 1) = attr._zaxis[1];
  orient(2, 2) = attr._zaxis[2];
  
  //offset vector
  irtkVector offset(4);
  offset(0)= attr._xorigin;
  offset(1)= attr._yorigin;
  offset(2)= attr._zorigin;
  offset(3)= 1;

  //needs to be rotated
  offset = orient*offset;

  //adjust translation 
  orient(0,3)=-offset(0);
  orient(1,3)=-offset(1);
  orient(2,3)=-offset(2);

  if (swap)
  {
    //matrix to swap x and y axis
    irtkMatrix axis_swap(4,4);
    axis_swap.Ident();
    axis_swap(0,0)=0;
    axis_swap(1,1)=0;
    axis_swap(0,1)=1;
    axis_swap(1,0)=1;
  
    //multiply the matrices
    orient = axis_swap*orient;
  }
  
  //create affine transformation
  irtkAffineTransformation tr;
  tr.PutMatrix(orient);
  tr.irtkTransformation::Write("adjusted-orient.dof");
  return tr;

}


void irtkReconstructionb0::ShimDistortion(irtkRealImage &acquired, irtkRealImage &simulated, irtkAffineTransformation &shim, bool swap)
{ 
  
  irtkRealImage acq,sim;
  acq = AdjustOrientation(acquired,swap);
  sim = AdjustOrientation(simulated,swap);
  
  irtkAffineTransformation orient = AdjustOrientationTransformation(acquired,swap);
  orient.irtkTransformation::Write("orient.dof");
  acq.Write("adjusted.nii.gz");
  sim.Write("simulated.nii.gz");
  
  //constrain distortion transformation
  irtkAffineTransformation distortion;
  //distortion.PutStatus(TX,  _Passive);
  distortion.PutStatus(TY,  _Passive);
  distortion.PutStatus(TZ,  _Passive);
  distortion.PutStatus(RX,  _Passive);
  distortion.PutStatus(RY,  _Passive);
  distortion.PutStatus(RZ,  _Passive);
  distortion.PutStatus(SY,  _Passive);
  distortion.PutStatus(SZ,  _Passive);
  distortion.PutStatus(SYZ, _Passive);

  double xsize,ysize,zsize;
  _reconstructed.GetPixelSize(&xsize, &ysize, &zsize);
  irtkImageAffineRegistrationWithPadding registration;
  irtkImageTransformation imagetransformation; 
  irtkLinearInterpolateImageFunction interpolator;
  irtkGreyImage t,s;
  t=acq;
  s=sim;
  registration.SetInput(&t,&s);
  registration.SetOutput(&distortion);
  registration.GuessParameterDistortion(xsize);
  registration.SetTargetPadding(0);
  registration.Run();
  distortion.irtkTransformation::Write("d.dof");
  registration.Write("par-shim.areg");
  
  irtkMatrix mo = orient.GetMatrix();
  irtkMatrix md = distortion.GetMatrix();
  md = md*mo;
  mo.Invert();
  md = mo*md;
  distortion.PutMatrix(md);
  shim.PutMatrix(md);

  distortion.irtkTransformation::Write("shim.dof");
  
  //return distortion;
  //irtkMatrix ms = _shim.GetMatrix();
  //ms=ms*md;
  //_shim.PutMatrix(ms);
  //_shim.irtkTransformation::Write("shim.dof");

}

void irtkReconstructionb0::Shim(vector<irtkRealImage> &stacks, int iter)
{
  cout<<"Shimming."<<endl;
  cout.flush();
  if(stacks.size()==0)
  {
    cerr<<"irtkReconstructionb0: Please set the stacks!"<<endl;
    exit(1);
  }
  vector<irtkRealImage> simulated;
  vector<irtkRealImage> stacks2;
  
  int ind;
  int i,j,k;
  char buffer[256];
  irtkRealImage image;
  double valst,valsim;
  
  //simulate stacks
  for(ind = 0; ind<stacks.size();ind++)
  {
    simulated.push_back(stacks[ind]);
    stacks2.push_back(stacks[ind]);
  }
  SimulateStacks(simulated);
  for(ind = 0; ind<stacks.size();ind++)
  {
    sprintf(buffer,"st%i.nii.gz",ind);
    stacks[ind].Write(buffer);
    sprintf(buffer,"sim%i.nii.gz",ind);
    simulated[ind].Write(buffer);
  }
  
  
  irtkImageTransformation *imagetransformation = new irtkImageTransformation;
  irtkImageFunction *interpolator = new irtkNearestNeighborInterpolateImageFunction;
  irtkImageFunction *interpolatorLin = new irtkLinearInterpolateImageFunction;
  imagetransformation->PutInterpolator(interpolator);
  //target probably has padding 0, need padding -1
  imagetransformation->PutTargetPaddingValue(-1);
  //need to fill voxels in target where there is no info from source with zeroes
  imagetransformation->PutSourcePaddingValue(0);

  cout<<"done."<<endl;
  cout.flush();
  cout<<"Groups: ";
  cout.flush();
  for(int g=0; g<_groups.size();g++)
  {
    cout<<g<<" ";
    cout.flush();
    //find all the stacks in the group g
    vector<int> stacknum;
    for(ind = 0; ind<stacks.size();ind++)
      if(_stack_group[ind]==_groups[g])
	stacknum.push_back(ind);

    irtkRealImage templateStack = stacks[stacknum[0]];
    irtkImageAttributes attr = templateStack.GetImageAttributes();
    attr._t = stacknum.size();
    cout<<endl<<"We have "<<attr._t<<" images in group "<<_groups[g]<<endl;
    irtkRealImage stack(attr), simul(attr);
  
    irtkRigidTransformation id;
    
    irtkRealImage st(templateStack),sim(templateStack);
    //resample all on the same grid
    for(int ind=0; ind<stacknum.size(); ind++)
    {
      imagetransformation->SetInput(&stacks[stacknum[ind]], &id);
      imagetransformation->SetOutput(&st);
      imagetransformation->Run();
    
      imagetransformation->SetInput(&simulated[stacknum[ind]], &id);
      imagetransformation->SetOutput(&sim);
      imagetransformation->Run();
    
      for(i=0;i<templateStack.GetX();i++)
        for(j=0;j<templateStack.GetY();j++)
          for(k=0;k<templateStack.GetZ();k++)
	  {
	    valst = st(i,j,k);
	    valsim = sim(i,j,k);
	    if(valst>0.01)
	      stack(i,j,k,ind)=valst;
	    else
	      stack(i,j,k,ind)=0;
	    if(valsim>0.01)
	      simul(i,j,k,ind)=valsim; 
	    else
	      simul(i,j,k,ind)=0;
	    /*
	    if ((valst>0)&&(valsim>0))
	    {
	      stack(i,j,k,ind)=valst;
	      simul(i,j,k,ind)=valsim; 
	    }
	    else
	    {
	      stack(i,j,k,ind)=0;
	      simul(i,j,k,ind)=0;
	    }
	    */
	  }
    }
    
    sprintf(buffer,"stacks%i-%i.nii.gz",iter,g);
    stack.Write(buffer);
    sprintf(buffer,"sims%i-%i.nii.gz",iter,g);
    simul.Write(buffer);
    
    //calculate shim
    irtkAffineTransformation shim;
    //ShimDistortion(stack,simul,shim,false);
    
    //if(g==0)
      ShimDistortion(stack,simul,shim,_swap[g]);
    //else
      //ShimDistortion(stack,simul,shim,true);
    //sprintf(buffer,"ishim%i.nii.gz",g);
    //shim.irtkTransformation::Write(buffer);
    shim.Invert();
    shim.UpdateParameter();
    shim.Print();
    sprintf(buffer,"shim%i-%i.dof",iter,g);
    shim.irtkTransformation::Write(buffer);
    
     imagetransformation->PutInterpolator(interpolatorLin);
    
    for(int ind=0; ind<stacknum.size(); ind++)
    {
      //sprintf(buffer,"s1-%i.nii.gz",stacknum[ind]);
      //stacks[stacknum[ind]].Write(buffer);
      //sprintf(buffer,"s2-%i.nii.gz",stacknum[ind]);
      //stacks2[stacknum[ind]].Write(buffer);
      cout<<"Correcting stack "<<stacknum[ind]<<endl;
      imagetransformation->SetInput(&stacks2[stacknum[ind]], &shim);
      imagetransformation->SetOutput(&stacks[stacknum[ind]]);
      imagetransformation->Run();
      
      //sprintf(buffer,"ss1-%i.nii.gz",stacknum[ind]);
      //stacks[stacknum[ind]].Write(buffer);
      //sprintf(buffer,"ss2-%i.nii.gz",stacknum[ind]);
      //stacks2[stacknum[ind]].Write(buffer);
    }
  }
  delete imagetransformation;
  delete interpolator;
  delete interpolatorLin;
}

void irtkReconstructionb0::FieldMap(vector<irtkRealImage> &stacks, int iter)
{
  cout<<"Field map correction."<<endl;
  cout<<"FieldMap correction: Warning: only implemented for stacks of same geometry at present!."<<endl;
  cout.flush();
  if(stacks.size()==0)
  {
    cerr<<"irtkReconstructionb0: Please set the stacks!"<<endl;
    exit(1);
  }
  vector<irtkRealImage> simulated;
  vector<irtkRealImage> stacks2;
  
  int ind;
  int i,j,k;
  char buffer[256];
  irtkRealImage image;
  double valst,valsim;
  
  //simulate stacks
  for(ind = 0; ind<stacks.size();ind++)
  {
    simulated.push_back(stacks[ind]);
    stacks2.push_back(stacks[ind]);
  }
  SimulateStacks(simulated);
  for(ind = 0; ind<stacks.size();ind++)
  {
    sprintf(buffer,"st%i.nii.gz",ind);
    stacks[ind].Write(buffer);
    sprintf(buffer,"sim%i.nii.gz",ind);
    simulated[ind].Write(buffer);
  }
  
  //Resample stacks and simulated to the same geometry and create 4D nifti
  irtkImageTransformation *imagetransformation = new irtkImageTransformation;
  irtkImageFunction *interpolator = new irtkNearestNeighborInterpolateImageFunction;
  irtkImageFunction *interpolatorLin = new irtkLinearInterpolateImageFunction;
  imagetransformation->PutInterpolator(interpolator);
  //target probably has padding 0, need padding -1
  imagetransformation->PutTargetPaddingValue(-1);
  //need to fill voxels in target where there is no info from source with zeroes
  imagetransformation->PutSourcePaddingValue(0);


  irtkRealImage templateStack = stacks[0];
  irtkImageAttributes attr = templateStack.GetImageAttributes();
  attr._t = stacks.size();
  cout<<endl<<"We have "<<attr._t<<" images "<<endl;
  irtkRealImage stack(attr), simul(attr);
  
  irtkRigidTransformation id;
    
  irtkRealImage st(templateStack),sim(templateStack);
  //resample all on the same grid
  for(int ind=0; ind<stacks.size(); ind++)
  {
    imagetransformation->SetInput(&stacks[ind], &id);
    imagetransformation->SetOutput(&st);
    imagetransformation->Run();
    
    imagetransformation->SetInput(&simulated[ind], &id);
    imagetransformation->SetOutput(&sim);
    imagetransformation->Run();
    
    for(i=0;i<templateStack.GetX();i++)
      for(j=0;j<templateStack.GetY();j++)
        for(k=0;k<templateStack.GetZ();k++)
	{
	  valst = st(i,j,k);
	  valsim = sim(i,j,k);
	  if(valst>0.01)
	    stack(i,j,k,ind)=valst;
	  else
	    stack(i,j,k,ind)=0;
	  if(valsim>0.01)
	    simul(i,j,k,ind)=valsim; 
	  else
	    simul(i,j,k,ind)=0;
	}
  }
  
  if (_debug)
  {
    sprintf(buffer,"fmstacks%i.nii.gz",iter);
    stack.Write(buffer);
    sprintf(buffer,"fmsims%i.nii.gz",iter);
    simul.Write(buffer);
  }
    
  //calculate b0 field distortiom
  irtkMultiLevelFreeFormTransformation dist;
  FieldMapDistortion(stack,simul,dist,_swap[0]);
  sprintf(buffer,"fmdist%i.dof",iter);
  dist.irtkTransformation::Write(buffer);
    
  //Corect the stacks
  imagetransformation->PutInterpolator(interpolatorLin); 
  for(int ind=0; ind<stacks.size(); ind++)
  {
    cout<<"Correcting stack "<<ind<<endl;
    imagetransformation->SetInput(&stacks2[ind], &dist);
    imagetransformation->SetOutput(&stacks[ind]);
    imagetransformation->Run();
  }
  
  delete imagetransformation;
  delete interpolator;
  delete interpolatorLin;
}

void  irtkReconstructionb0::FieldMapDistortion(irtkRealImage &stack,irtkRealImage &simul, irtkMultiLevelFreeFormTransformation &distortion, bool swap)
{
  //Adjust orientation
  irtkRealImage st,sm;
  sm = AdjustOrientation(simul,false);
  st = AdjustOrientation(stack,false);
  if (_debug)
  {
    sm.Write("sm.nii.gz");
    st.Write("st.nii.gz");
  }
  irtkAffineTransformation orient = AdjustOrientationTransformation(simul,false);
  orient.irtkTransformation::Write("orient.dof");
  
  //register acquired stacks to simulated
  irtkImageFreeFormRegistrationWithPadding registration;
  if(swap)
    registration.SetMode(RegisterY);
  else
    registration.SetMode(RegisterX);
  
  irtkGreyImage t,s;
  t=sm;
  s=st;
  registration.SetInput(&t,&s);
  registration.SetOutput(&distortion);
  registration.GuessParameterDistortion(1,10);
  registration.irtkImageRegistration::Write("par-dist.nreg");
  registration.SetTargetPadding(0);
  registration.Run();
  distortion.irtkTransformation::Write("fmd.dof");
  
  //adjust lattice of Bspine transformation according to the original images
  irtkImageAttributes attr = simul.GetImageAttributes();
  irtkFreeFormTransformation3D *bspline = dynamic_cast<irtkFreeFormTransformation3D *>(distortion.GetLocalTransformation(0));
  bspline->PutOrigin(attr._xorigin, attr._yorigin, attr._zorigin);
  bspline->PutOrientation(attr._xaxis, attr._yaxis, attr._zaxis);
  
  //orient Bspline control points
  irtkMatrix mo = orient.GetMatrix();
  mo.Invert();

  irtkVector cp(4);
  cp(3)=0;
  for (int k = 0; k < bspline->GetZ(); ++k) {
    for (int j = 0; j < bspline->GetY(); ++j) {
      for (int i = 0; i < bspline->GetX(); ++i) {
        bspline->Get(i, j, k, cp(0), cp(1), cp(2));
	cp=mo*cp;
        bspline->Put(i, j, k, cp(0), cp(1), cp(2));
      }
    }  
  }
  distortion.irtkTransformation::Write("fmdist.dof");
}


irtkRealImage irtkReconstructionb0::Create4DImage(vector<irtkRealImage> &stacks)
{
  int i,j,k;
  double val;
  irtkImageTransformation *imagetransformation = new irtkImageTransformation;
  irtkImageFunction *interpolator = new irtkNearestNeighborInterpolateImageFunction;
  irtkImageFunction *interpolatorLin = new irtkLinearInterpolateImageFunction;
  imagetransformation->PutInterpolator(interpolator);
  //target probably has padding 0, need padding -1
  imagetransformation->PutTargetPaddingValue(-1);
  //need to fill voxels in target where there is no info from source with zeroes
  imagetransformation->PutSourcePaddingValue(0);
  
  irtkRealImage templateStack = stacks[0];
  irtkImageAttributes attr = templateStack.GetImageAttributes();
  attr._t = stacks.size();
  irtkRealImage stack(attr);
  
  irtkRigidTransformation id;
  irtkRealImage st(templateStack);
  //resample all on the same grid
  for(int ind=0; ind<stacks.size(); ind++)
  {
    imagetransformation->SetInput(&stacks[ind], &id);
    imagetransformation->SetOutput(&st);
    imagetransformation->Run();
    
    for(i=0;i<templateStack.GetX();i++)
      for(j=0;j<templateStack.GetY();j++)
        for(k=0;k<templateStack.GetZ();k++)
        {
	  val = st(i,j,k);
	  if (val>0)
	    stack(i,j,k,ind)=val;
	  else
	  stack(i,j,k,ind)=0;
	}
    }
    stack.Write("4D.nii.gz");
    return stack;
  
}

void irtkReconstructionb0::CreateSimulated(vector<irtkRealImage> &stacks)
{
  _simulated.clear();
  for (int i=0;i<stacks.size();i++)
  {
    _simulated.push_back(stacks[i]);
    _simulated[i]=0;
  }
  SimulateStacks(_simulated);
}

void irtkReconstructionb0::WriteSimulated()
{
  char buffer[256];
  for(int i=0;i<_simulated.size();i++)
  {
    sprintf(buffer,"simulated%i.nii.gz",i);
    _simulated[i].Write(buffer);
  }
}